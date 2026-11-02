// Compile-fail case for the server's list paging types (tests/CMakeLists.txt): an
// offset and a limit cannot be swapped, nor passed as bare integers.
#include "db.hpp"

void lm_compile_fail_case(lm::server::Db &db);
void lm_compile_fail_case(lm::server::Db &db) {
#if LM_CF_CASE == 1
    (void)db.json_albums("title", lm::server::PageLimit(100), lm::server::PageOffset(0));
#elif LM_CF_CASE == 2
    (void)db.json_albums("title", 0, 100);
#else
#error "LM_CF_CASE not set"
#endif
}
