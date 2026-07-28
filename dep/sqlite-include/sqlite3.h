#pragma once

// Keep dep/sqlite itself out of consumers' header search paths. On Windows,
// SQLite's VERSION file otherwise shadows the C++ standard <version> header.
#include "../sqlite/sqlite3.h"
