#pragma once
/**
 * test_weld_path.h -- declarations for test_weld_path.cpp, so
 * test_contract.cpp's main() can RUN_TEST() them. See that .cpp's header
 * comment for what this suite covers.
 */

void test_weldPath_singleOpenStroke(void);
void test_weldPath_shortStrokeSkipped(void);
void test_weldPath_closedStrokeWrapsToStart(void);
void test_weldPath_emptyInputReturnsZero(void);
void test_weldPath_nodeCapacityClamped(void);
void test_weldPath_liftCapacityClamped(void);
void test_weldPath_sampleOutOfRangeInvalid(void);
void test_weldPath_sampleInterpolatesOnSegment(void);
void test_weldPath_crossesLiftDetectsGapBetweenStrokes(void);
void test_weldPath_crossesLiftFalseWithinOneStroke(void);

// Text-Welding regression scenarios (see weld_patterns.cpp's generateText()):
// glyphOutlinePaths() hands weld one SourceStroke per pen-lift sub-path,
// exactly like these synthetic "glyph" strokes -- covers short text (one
// glyph), longer text (many glyphs), and the combination with the real
// point_optimizer.
void test_weldText_shortStringSingleGlyph(void);
void test_weldText_longerStringManyGlyphsNoConnectingLine(void);
void test_weldText_optimizerProducesNoInvalidPointsAndBlanksBetweenGlyphs(void);
