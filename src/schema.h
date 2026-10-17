// ============================================================================
//  schema.h —— 命令与选项的唯一来源
//
//  以前选项散落在四个地方：IsValueFlag / PrintUsage / PrintSchema / README。
//  对不上是必然的，而且这个工具靠"模型自己读 schema 学会用"，
//  schema 一旦撒谎，模型就会用错参数。
//
//  现在只有这一张表，三处输出都从它生成：
//    - args 解析时判断"这个开关后面跟不跟值"
//    - --help 的输出
//    - schema 的 JSON
// ============================================================================
#pragma once

#include <string>
#include <vector>

struct OptSpec {
    const wchar_t* name;
    bool           takesValue;
    const wchar_t* help;
};

struct CmdSpec {
    const wchar_t* name;
    const wchar_t* usage;    // "see [选项]"
    const wchar_t* desc;
    const OptSpec* opts;     // 以 { nullptr, false, nullptr } 结尾
};

// 所有命令（按帮助里的顺序）
const std::vector<CmdSpec>& CommandSpecs();

// 这个开关后面是否跟一个值（用于把位置参数和选项值分开）
bool OptTakesValue(const std::wstring& name);

void PrintUsageFromTable();
void PrintSchemaFromTable();
