// tests/commands/model_command_test.cpp
//
// 覆盖 src/commands/model_command.cpp 的 args 解析子命令分支。
// 新加的 add / edit / rm / set-default 每个都有独立的写盘副作用,解析失
// 败应当不动 cfg。
//
// 触发场景 / 期望:
//   - /model add key=val ... → sub="add", kvs 完整
//   - /model rm <name>       → sub="rm", name=<name>
//   - /model set-default <n> → sub="set-default", name=<n>
//   - /model <name>          → 无 sub,name 直接是模型名
//   - /model --cwd <name>    → flag="--cwd", name=<name>

#include <gtest/gtest.h>

#include "commands/model_command.hpp"

// 场景:/model add 完整 kv 列表 → 解析后 kvs 字段就绪。
TEST(ModelCommandParse, ParsesAddSubcommand) {
    acecode::ParsedModelSub p;
    EXPECT_TRUE(acecode::parse_model_subcommand(
        "add name=foo provider=openai model=llama base_url=http://x api_key=k", p));
    EXPECT_EQ(p.sub, "add");
    EXPECT_EQ(p.kvs["name"], "foo");
    EXPECT_EQ(p.kvs["provider"], "openai");
    EXPECT_EQ(p.kvs["api_key"], "k");
}

// 场景:/model rm foo → sub="rm",name="foo"。
TEST(ModelCommandParse, ParsesRmSubcommand) {
    acecode::ParsedModelSub p;
    EXPECT_TRUE(acecode::parse_model_subcommand("rm foo", p));
    EXPECT_EQ(p.sub, "rm");
    EXPECT_EQ(p.name, "foo");
}

// 场景:/model set-default foo → sub="set-default"。
TEST(ModelCommandParse, ParsesSetDefault) {
    acecode::ParsedModelSub p;
    EXPECT_TRUE(acecode::parse_model_subcommand("set-default foo", p));
    EXPECT_EQ(p.sub, "set-default");
    EXPECT_EQ(p.name, "foo");
}

// 场景:/model gpt-4o(无 sub) → sub 空,name="gpt-4o"。
// 与原 model_command.cpp 的 parse_model_args 行为兼容。
TEST(ModelCommandParse, ParsesBareNameWithoutSubcommand) {
    acecode::ParsedModelSub p;
    EXPECT_TRUE(acecode::parse_model_subcommand("gpt-4o", p));
    EXPECT_EQ(p.sub, "");
    EXPECT_EQ(p.name, "gpt-4o");
}

// 场景:/model --cwd gpt-4o → flag="--cwd"。
TEST(ModelCommandParse, ParsesCwdFlag) {
    acecode::ParsedModelSub p;
    EXPECT_TRUE(acecode::parse_model_subcommand("--cwd gpt-4o", p));
    EXPECT_EQ(p.flag, "--cwd");
    EXPECT_EQ(p.name, "gpt-4o");
}
