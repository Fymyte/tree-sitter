#include "spec_helper.h"
#include "runtime/alloc.h"
#include "helpers/load_language.h"
#include "compiler/util/string_helpers.h"
#include <map>

static string dedent(string input) {
  size_t indent_level = input.find_first_not_of("\n ") - input.find_first_not_of("\n");
  string whitespace = "\n" + string(indent_level, ' ');
  util::str_replace(&input, whitespace, "\n");
  return input.substr(
    input.find_first_not_of("\n "),
    input.find_last_not_of("\n ") + 1
  );
}

static string fill_template(string input, map<string, string> parameters) {
  string result = input;
  for (const auto &pair : parameters) {
    util::str_replace(&result, "{{" + pair.first + "}}", pair.second);
  }
  return result;
}

START_TEST

describe("compile_grammar", []() {
  TSDocument *document;

  before_each([&]() {
    document = ts_document_new();
  });

  after_each([&]() {
    ts_document_free(document);
  });

  auto assert_root_node = [&](const string &expected_string) {
    TSNode root_node = ts_document_root_node(document);
    char *node_string = ts_node_string(root_node, document);
    AssertThat(node_string, Equals(expected_string));
    ts_free(node_string);
  };

  describe("conflicts", [&]() {
    it("can resolve shift/reduce conflicts using associativities", [&]() {
      string grammar_template = R"JSON({
        "name": "associativity_example",

        "rules": {
          "expression": {
            "type": "CHOICE",
            "members": [
              {"type": "SYMBOL", "name": "math_operation"},
              {"type": "SYMBOL", "name": "identifier"}
            ]
          },

          "math_operation": {
            "type": "{{math_operation_prec_type}}",
            "value": 0,
            "content": {
              "type": "SEQ",
              "members": [
                {"type": "SYMBOL", "name": "expression"},
                {"type": "STRING", "value": "+"},
                {"type": "SYMBOL", "name": "expression"}
              ]
            }
          },

          "identifier": {
            "type": "PATTERN",
            "value": "[a-zA-Z]+"
          }
        }
      })JSON";

      // Ambiguity, which '+' applies first?
      ts_document_set_input_string(document, "x+y+z");

      TSCompileResult result = ts_compile_grammar(fill_template(grammar_template, {
        {"math_operation_prec_type", "PREC"}
      }).c_str());

      AssertThat(result.error_message, Equals(dedent(R"MESSAGE(
        Unresolved conflict for symbol sequence:

          expression  '+'  expression  •  '+'  …

        Possible interpretations:

          (math_operation  expression  '+'  expression)  •  '+'  …

          expression  '+'  (math_operation  expression  •  '+'  expression)

        Possible resolutions:

          Specify left or right associativity in the rules:  math_operation

          Add a conflict for the rules:  math_operation
      )MESSAGE")));

      result = ts_compile_grammar(fill_template(grammar_template, {
        {"math_operation_prec_type", "PREC_LEFT"}
      }).c_str());

      ts_document_set_language(document, load_compile_result("associativity_example", result));
      ts_document_parse(document);
      assert_root_node("(expression (math_operation "
        "(expression (math_operation (expression (identifier)) (expression (identifier)))) "
        "(expression (identifier))))");

      result = ts_compile_grammar(fill_template(grammar_template, {
        {"math_operation_prec_type", "PREC_RIGHT"}
      }).c_str());

      ts_document_set_language(document, load_compile_result("associativity_example", result));
      ts_document_parse(document);
      assert_root_node("(expression (math_operation "
        "(expression (identifier)) "
        "(expression (math_operation (expression (identifier)) (expression (identifier))))))");
    });

    it("can resolve shift/reduce conflicts involving single-child rules using precedence", [&]() {
      string grammar_template = R"JSON({
        "name": "associativity_example",

        "extras": [
          {"type": "PATTERN", "value": "\\s"}
        ],

        "rules": {
          "expression": {
            "type": "CHOICE",
            "members": [
              {"type": "SYMBOL", "name": "function_call"},
              {"type": "SYMBOL", "name": "identifier"}
            ]
          },

          "function_call": {
            "type": "PREC_RIGHT",
            "value": {{function_call_precedence}},
            "content": {
              "type": "CHOICE",
              "members": [
                {
                  "type": "SEQ",
                  "members": [
                    {"type": "SYMBOL", "name": "identifier"},
                    {"type": "SYMBOL", "name": "expression"}
                  ]
                },
                {
                  "type": "SEQ",
                  "members": [
                    {"type": "SYMBOL", "name": "identifier"},
                    {"type": "SYMBOL", "name": "block"}
                  ]
                },
                {
                  "type": "SEQ",
                  "members": [
                    {"type": "SYMBOL", "name": "identifier"},
                    {"type": "SYMBOL", "name": "expression"},
                    {"type": "SYMBOL", "name": "block"}
                  ]
                }
              ]
            }
          },

          "block": {
            "type": "SEQ",
            "members": [
              {"type": "STRING", "value": "{"},
              {"type": "SYMBOL", "name": "expression"},
              {"type": "STRING", "value": "}"}
            ]
          },

          "identifier": {
            "type": "PATTERN",
            "value": "[a-zA-Z]+"
          }
        }
      })JSON";

      // Ambiguity: is the trailing block associated with `bar` or `foo`?
      ts_document_set_input_string(document, "foo bar { baz }");

      TSCompileResult result = ts_compile_grammar(fill_template(grammar_template, {
        {"function_call_precedence", "0"}
      }).c_str());

      AssertThat(result.error_message, Equals(dedent(R"MESSAGE(
        Unresolved conflict for symbol sequence:

          identifier  •  '{'  …

        Possible interpretations:

          (expression  identifier)  •  '{'  …

          (function_call  identifier  •  block)

        Possible resolutions:

          Use different precedences in the rules:  expression  function_call

          Specify left or right associativity in the rules:  expression

          Add a conflict for the rules:  expression  function_call
      )MESSAGE")));

      // Giving function calls lower precedence than expressions causes `bar`
      // to be treated as an expression passed to `foo`, not as a function
      // that's being called with a block.
      result = ts_compile_grammar(fill_template(grammar_template, {
        {"function_call_precedence", "-1"}
      }).c_str());

      AssertThat(result.error_message, IsNull());
      ts_document_set_language(document, load_compile_result("associativity_example", result));
      ts_document_parse(document);
      assert_root_node("(expression (function_call "
        "(identifier) "
        "(expression (identifier)) "
        "(block (expression (identifier)))))");

      // Giving function calls higher precedence than expressions causes `bar`
      // to be treated as a function that's being called with a block, not as
      // an expression passed to `foo`.
      result = ts_compile_grammar(fill_template(grammar_template, {
        {"function_call_precedence", "1"}
      }).c_str());

      AssertThat(result.error_message, IsNull());
      ts_document_set_language(document, load_compile_result("associativity_example", result));
      ts_document_set_input_string(document, "foo bar { baz }");
      ts_document_parse(document);
      assert_root_node("(expression (function_call "
        "(identifier) "
        "(expression (function_call "
          "(identifier) "
          "(block (expression (identifier)))))))");
    });
  });

  describe("when the grammar's start symbol is a token", [&]() {
    it("parses the token", [&]() {
      TSCompileResult result = ts_compile_grammar(R"JSON(
        {
          "name": "one_token_language",
          "rules": {
            "first_rule": {"type": "STRING", "value": "the-value"}
          }
        }
      )JSON");

      ts_document_set_language(document, load_compile_result("one_token_language", result));

      ts_document_set_input_string(document, "the-value");
      ts_document_parse(document);
      assert_root_node("(first_rule)");
    });
  });

  describe("when the grammar's start symbol is blank", [&]() {
    it("parses the empty string", [&]() {
      TSCompileResult result = ts_compile_grammar(R"JSON(
        {
          "name": "blank_language",
          "rules": {
            "first_rule": {"type": "BLANK"}
          }
        }
      )JSON");

      ts_document_set_language(document, load_compile_result("blank_language", result));

      ts_document_set_input_string(document, "");
      ts_document_parse(document);
      assert_root_node("(first_rule)");
    });
  });

  describe("when the grammar contains anonymous tokens with escaped characters", [&]() {
    it("escapes the escaped characters properly in the generated parser", [&]() {
      TSCompileResult result = ts_compile_grammar(R"JSON(
        {
          "name": "escaped_char_language",
          "rules": {
            "first_rule": {
              "type": "CHOICE",
              "members": [
                {"type": "STRING", "value": "\n"},
                {"type": "STRING", "value": "\r"},
                {"type": "STRING", "value": "'hello'"},
                {"type": "PATTERN", "value": "\\d+"}
              ]
            }
          }
        }
      )JSON");

      ts_document_set_language(document, load_compile_result("escaped_char_language", result));

      ts_document_set_input_string(document, "1234");
      ts_document_parse(document);
      assert_root_node("(first_rule)");

      ts_document_set_input_string(document, "\n");
      ts_document_parse(document);
      assert_root_node("(first_rule)");

      ts_document_set_input_string(document, "'hello'");
      ts_document_parse(document);
      assert_root_node("(first_rule)");
    });
  });

  describe("the grammar in the README", [&]() {
    it("parses the input in the README", [&]() {
      TSCompileResult result = ts_compile_grammar(R"JSON(
        {
          "name": "arithmetic",

          // Things that can appear anywhere in the language, like comments
          // and whitespace, are expressed as 'extras'.
          "extras": [
            {"type": "PATTERN", "value": "\\s"},
            {"type": "SYMBOL", "name": "comment"}
          ],

          "rules": {

            // The first rule listed in the grammar becomes the 'start rule'.
            "expression": {
              "type": "CHOICE",
              "members": [
                {"type": "SYMBOL", "name": "sum"},
                {"type": "SYMBOL", "name": "product"},
                {"type": "SYMBOL", "name": "number"},
                {"type": "SYMBOL", "name": "variable"},
                {
                  "type": "SEQ",
                  "members": [
                    {"type": "STRING", "value": "("},
                    {"type": "SYMBOL", "name": "expression"},
                    {"type": "STRING", "value": ")"}
                  ]
                }
              ]
            },

            // Tokens like '+' and '*' are described directly within the
            // grammar's rules, as opposed to in a seperate lexer description.
            "sum": {
              "type": "PREC_LEFT",
              "value": 1,
              "content": {
                "type": "SEQ",
                "members": [
                  {"type": "SYMBOL", "name": "expression"},
                  {"type": "STRING", "value": "+"},
                  {"type": "SYMBOL", "name": "expression"}
                ]
              }
            },

            // Ambiguities can be resolved at compile time by assigning precedence
            // values to rule subtrees.
            "product": {
              "type": "PREC_LEFT",
              "value": 2,
              "content": {
                "type": "SEQ",
                "members": [
                  {"type": "SYMBOL", "name": "expression"},
                  {"type": "STRING", "value": "*"},
                  {"type": "SYMBOL", "name": "expression"}
                ]
              }
            },

            // Tokens can be specified using ECMAScript regexps.
            "number": {"type": "PATTERN", "value": "\\d+"},
            "comment": {"type": "PATTERN", "value": "#.*"},
            "variable": {"type": "PATTERN", "value": "[a-zA-Z]\\w*"}
          }
        }
      )JSON");

      const TSLanguage *language = load_compile_result("arithmetic", result);

      ts_document_set_language(document, language);
      ts_document_set_input_string(document, "a + b * c");
      ts_document_parse(document);

      assert_root_node(
        "(expression (sum "
          "(expression (variable)) "
          "(expression (product "
             "(expression (variable)) "
             "(expression (variable))))))");
    });
  });
});

END_TEST
