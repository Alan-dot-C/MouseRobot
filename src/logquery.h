// ============================================================================
//  logquery.h —— `handeye log` 查询命令
// ============================================================================
#pragma once

#include <string>
#include <vector>

// rawArgs 是完整命令行参数（含 "log" 本身）
int CmdLogQuery(const std::vector<std::wstring>& rawArgs);
