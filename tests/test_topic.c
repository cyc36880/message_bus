/**
 * @file test_topic.c
 * @brief 主题校验与通配符匹配（MQTT 3.1.1 规则）。
 */
#include "mb_test.h"
#include "message_bus/mb_topic.h"

static void expect_match(const char *filter, const char *topic, bool expected)
{
    bool actual = mb_topic_match(filter, topic);

    mb_test_checks++;
    if (actual != expected) {
        mb_test_failures++;
        printf("    match mismatch: filter='%s' topic='%s' actual=%d expected=%d\n",
               filter, topic, (int)actual, (int)expected);
    }
}

static void expect_filter_err(const char *filter, mb_err_t expected)
{
    MB_CHECK_INT(mb_topic_validate_filter(filter), expected);
}

static void expect_topic_err(const char *topic, mb_err_t expected)
{
    MB_CHECK_INT(mb_topic_validate_topic(topic), expected);
}

MB_TEST(topic_exact_match)
{
    expect_match("a/b/c", "a/b/c", true);
    expect_match("a/b/c", "a/b/d", false);
    expect_match("a", "a", true);
    expect_match("a", "a/b", false);
    expect_match("a/b", "a", false);
    expect_match("/a", "/a", true);
    expect_match("/a", "a", false);
    expect_match("a//b", "a//b", true);
}

MB_TEST(topic_single_wildcard)
{
    expect_match("a/+/c", "a/b/c", true);
    expect_match("a/+/c", "a/b/d", false);
    expect_match("a/+/c", "a/b/x/c", false);
    expect_match("+", "a", true);
    expect_match("+", "a/b", false);
    expect_match("a/+", "a", false);
    expect_match("+/+/+", "a/b/c", true);
    /* '+' 能匹配空层级 */
    expect_match("a/+/c", "a//c", true);
}

MB_TEST(topic_multi_wildcard)
{
    expect_match("#", "a", true);
    expect_match("#", "a/b/c", true);
    expect_match("a/#", "a", true); /* '#' 包含父层级 */
    expect_match("a/#", "a/b", true);
    expect_match("a/#", "a/b/c/d", true);
    expect_match("a/#", "b", false);
    expect_match("a/b/#", "a/b", true);
    expect_match("a/b/#", "a/b/c", true);
    expect_match("a/b/#", "a/c", false);
}

MB_TEST(topic_system_prefix)
{
    /* '$' 开头的主题不被首层通配符匹配 [MQTT-4.7.2-1] */
    expect_match("#", "$mb/nodes/ui/connected", false);
    expect_match("+/nodes", "$mb/nodes", false);
    expect_match("$mb/#", "$mb/nodes/ui/connected", true);
    expect_match("$mb/nodes/+/connected", "$mb/nodes/ui/connected", true);
    expect_match("$mb/nodes/+/connected", "$mb/nodes/ui/disconnected", false);
}

MB_TEST(topic_match_null_and_empty)
{
    expect_match(NULL, "a", false);
    expect_match("a", NULL, false);
    expect_match("", "a", false);
    expect_match("a", "", false);
}

MB_TEST(topic_validate_filters)
{
    expect_filter_err("a/b/c", MB_OK);
    expect_filter_err("#", MB_OK);
    expect_filter_err("a/#", MB_OK);
    expect_filter_err("sport/+/player1", MB_OK);
    expect_filter_err("/", MB_OK);
    expect_filter_err("+/+/+", MB_OK);

    expect_filter_err(NULL, MB_ERR_INVALID_ARG);
    expect_filter_err("", MB_ERR_INVALID_ARG);
    expect_filter_err("a/#/b", MB_ERR_INVALID_ARG); /* '#' 必须是最后一层 */
    expect_filter_err("a#", MB_ERR_INVALID_ARG);    /* '#' 必须独占一层 */
    expect_filter_err("#/a", MB_ERR_INVALID_ARG);
    expect_filter_err("a+/b", MB_ERR_INVALID_ARG);  /* '+' 必须独占一层 */
    expect_filter_err("a/b+", MB_ERR_INVALID_ARG);
}

MB_TEST(topic_validate_publish_topic)
{
    expect_topic_err("motor/cmd", MB_OK);
    expect_topic_err("a", MB_OK);
    expect_topic_err("$mb/nodes/ui/connected", MB_OK);

    expect_topic_err(NULL, MB_ERR_INVALID_ARG);
    expect_topic_err("", MB_ERR_INVALID_ARG);
    expect_topic_err("motor/+", MB_ERR_INVALID_ARG); /* 发布主题不允许通配符 */
    expect_topic_err("motor/#", MB_ERR_INVALID_ARG);
}

MB_TEST(topic_too_long)
{
    char long_topic[MB_CONFIG_MAX_TOPIC_LEN + 8];
    size_t i;

    for (i = 0; i < sizeof(long_topic) - 1; ++i) {
        long_topic[i] = 'a';
    }
    long_topic[sizeof(long_topic) - 1] = '\0';

    MB_CHECK_INT(mb_topic_validate_topic(long_topic), MB_ERR_TOO_LONG);
    MB_CHECK_INT(mb_topic_validate_filter(long_topic), MB_ERR_TOO_LONG);
}

MB_TEST(topic_helpers)
{
    char buf[64];

    MB_CHECK_INT(mb_topic_level_count("a/b/c"), 3);
    MB_CHECK_INT(mb_topic_level_count("a"), 1);
    MB_CHECK_INT(mb_topic_level_count(""), 0);
    MB_CHECK_INT(mb_topic_level_count(NULL), 0);
    MB_CHECK_INT(mb_topic_level_count("a//b"), 3);

    MB_CHECK(mb_topic_is_system("$mb/x"));
    MB_CHECK(!mb_topic_is_system("mb/x"));
    MB_CHECK(!mb_topic_is_system(NULL));

    MB_CHECK_INT(mb_topic_build(buf, sizeof(buf), "motor", "cmd/speed"), MB_OK);
    MB_CHECK_STR(buf, "motor/cmd/speed");

    MB_CHECK_INT(mb_topic_build(buf, sizeof(buf), "motor", NULL), MB_OK);
    MB_CHECK_STR(buf, "motor");

    MB_CHECK_INT(mb_topic_build(buf, sizeof(buf), "motor", ""), MB_OK);
    MB_CHECK_STR(buf, "motor");

    MB_CHECK_INT(mb_topic_build(buf, 4, "motor", "cmd"), MB_ERR_TOO_LONG);
    MB_CHECK_INT(mb_topic_build(buf, sizeof(buf), NULL, "cmd"), MB_ERR_INVALID_ARG);
}

static const mb_test_case_t cases[] = {
    MB_CASE(topic_exact_match),
    MB_CASE(topic_single_wildcard),
    MB_CASE(topic_multi_wildcard),
    MB_CASE(topic_system_prefix),
    MB_CASE(topic_match_null_and_empty),
    MB_CASE(topic_validate_filters),
    MB_CASE(topic_validate_publish_topic),
    MB_CASE(topic_too_long),
    MB_CASE(topic_helpers),
};

const mb_test_suite_t *mb_suite_topic(void)
{
    static const mb_test_suite_t suite = { "topic (topics and wildcards)", cases, sizeof(cases) / sizeof(cases[0]) };

    return &suite;
}
