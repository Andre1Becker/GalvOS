#pragma once
/**
 * test_preset_matrix.h -- declarations for test_preset_matrix.cpp, so
 * test_contract.cpp's main() can RUN_TEST() them. See that .cpp's header
 * comment for what this suite covers and how it relates to CONTRACT.md's
 * invariant tests.
 */

void test_presetMatrixEasy(void);
void test_presetMatrixMedium(void);
void test_presetMatrixHard(void);
void test_presetMatrixInvalidParamsDegradeGracefully(void);
