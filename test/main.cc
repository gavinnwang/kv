#include "db.h"
#include "log.h"
#include <iostream>

int main() {
  std::cout << "Hello World" << std::endl;
  auto db_or_err = kv::DB::Open("./db.db");
  if (!db_or_err) {
    auto err = db_or_err.error();
    LOG_ERROR("DB open failed: {}", err.message());
    return 0;
  }

  auto db = std::move(*db_or_err);
  auto res = db->Begin(false);

  if (res) {
    LOG_INFO("db works fine");
  } else {
    LOG_INFO("db works not fine");
  }
  return 0;
}
