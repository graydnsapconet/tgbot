#include "test.h"
#include "llm.h"

#include <string.h>

// helper: copy string into mutable buffer and call llm_strip_think_tags
static void strip(const char *input, char *out, size_t out_cap, size_t *out_len)
{
    snprintf(out, out_cap, "%s", input);
    *out_len = llm_strip_think_tags(out);
}

// ── basic cases ──────────────────────────────────────────────────────

TEST(strip_no_tags)
{
    char buf[256];
    size_t len;
    strip("Hello, world!", buf, sizeof(buf), &len);
    ASSERT_STR_EQ(buf, "Hello, world!");
    ASSERT_EQ(len, strlen("Hello, world!"));
}

TEST(strip_simple_think_block)
{
    char buf[256];
    size_t len;
    strip("<think>reasoning here</think>Final answer.", buf, sizeof(buf), &len);
    ASSERT_STR_EQ(buf, "Final answer.");
}

TEST(strip_think_with_newlines)
{
    char buf[512];
    size_t len;
    strip("<think>\nline 1\nline 2\n</think>\nHello!", buf, sizeof(buf), &len);
    ASSERT_STR_EQ(buf, "Hello!");
}

TEST(strip_self_closing_think)
{
    char buf[256];
    size_t len;
    strip("<think/>The actual reply.", buf, sizeof(buf), &len);
    ASSERT_STR_EQ(buf, "The actual reply.");
}

TEST(strip_self_closing_with_space)
{
    char buf[256];
    size_t len;
    strip("<think />The actual reply.", buf, sizeof(buf), &len);
    ASSERT_STR_EQ(buf, "The actual reply.");
}

TEST(strip_multiple_think_blocks)
{
    char buf[512];
    size_t len;
    strip("<think>block1</think>Hello <think>block2</think>world!", buf, sizeof(buf), &len);
    ASSERT_STR_EQ(buf, "Hello world!");
}

TEST(strip_think_in_middle)
{
    char buf[256];
    size_t len;
    strip("Before <think>hidden</think> After", buf, sizeof(buf), &len);
    ASSERT_STR_EQ(buf, "Before  After");
}

TEST(strip_think_at_end)
{
    char buf[256];
    size_t len;
    strip("Reply text<think>trailing thought</think>", buf, sizeof(buf), &len);
    ASSERT_STR_EQ(buf, "Reply text");
}

// ── edge cases ───────────────────────────────────────────────────────

TEST(strip_empty_think_block)
{
    char buf[256];
    size_t len;
    strip("<think></think>Content here", buf, sizeof(buf), &len);
    ASSERT_STR_EQ(buf, "Content here");
}

TEST(strip_only_think_block)
{
    char buf[256];
    size_t len;
    strip("<think>only thinking</think>", buf, sizeof(buf), &len);
    ASSERT_EQ(len, (size_t)0);
    ASSERT_STR_EQ(buf, "");
}

TEST(strip_unclosed_think_tag)
{
    // unclosed <think> - strip everything after it
    char buf[256];
    size_t len;
    strip("Before <think>never closed", buf, sizeof(buf), &len);
    ASSERT_STR_EQ(buf, "Before");
}

TEST(strip_null_input)
{
    size_t len = llm_strip_think_tags(NULL);
    ASSERT_EQ(len, (size_t)0);
}

TEST(strip_empty_string)
{
    char buf[8] = "";
    size_t len = llm_strip_think_tags(buf);
    ASSERT_EQ(len, (size_t)0);
    ASSERT_STR_EQ(buf, "");
}

TEST(strip_whitespace_trimming)
{
    char buf[256];
    size_t len;
    strip("<think>thoughts</think>  \n  Hello!  \n  ", buf, sizeof(buf), &len);
    ASSERT_STR_EQ(buf, "Hello!");
}

TEST(strip_case_insensitive)
{
    char buf[256];
    size_t len;
    strip("<THINK>hidden</THINK>visible", buf, sizeof(buf), &len);
    ASSERT_STR_EQ(buf, "visible");
}

TEST(strip_mixed_case)
{
    char buf[256];
    size_t len;
    strip("<Think>hidden</Think>visible", buf, sizeof(buf), &len);
    ASSERT_STR_EQ(buf, "visible");
}

TEST(strip_angle_bracket_not_think)
{
    // ensure we don't strip non-think tags
    char buf[256];
    size_t len;
    strip("<b>bold</b> text", buf, sizeof(buf), &len);
    ASSERT_STR_EQ(buf, "<b>bold</b> text");
}

TEST(strip_partial_think_tag)
{
    // < followed by "thin" but not "think" - keep it
    char buf[256];
    size_t len;
    strip("<thin>not a think tag</thin>", buf, sizeof(buf), &len);
    ASSERT_STR_EQ(buf, "<thin>not a think tag</thin>");
}

TEST(strip_realistic_model_output)
{
    char buf[1024];
    size_t len;
    strip("<think>\nThe user is asking about the weather.\n"
          "I should provide a helpful response.\n"
          "</think>\n\n"
          "I don't have access to real-time weather data, "
          "but you can check your local weather service!",
          buf, sizeof(buf), &len);
    ASSERT_STR_EQ(buf,
                  "I don't have access to real-time weather data, "
                  "but you can check your local weather service!");
}

int main(void)
{
    printf("test_llm\n");
    return test_summarise();
}
