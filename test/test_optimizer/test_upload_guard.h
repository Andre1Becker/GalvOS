#pragma once
/**
 * test_upload_guard.h -- declarations for test_upload_guard.cpp, so
 * test_contract.cpp's main() can RUN_TEST() them. See that .cpp's header
 * comment for what this suite covers.
 */

void test_uploadGuard_allowsWhenDisarmed(void);
void test_uploadGuard_rejectsWhenArmed(void);
void test_uploadGuard_messageNamesTheAction(void);
