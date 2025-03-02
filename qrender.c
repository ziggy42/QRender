/*
 * Copyright 2025 Google LLC
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define VERSION 1
#define VERSION_1_SIDE_LENGTH 21
#define FINDER_PATTERN_SIZE_LENGTH 7
#define ENCODING_MODE_INDICATOR_BYTE 0b0100
#define QUIET_ZONE_SIZE 5

// Error Correction Level L (Low).
#define ERROR_CORRECTION_LEVEL 0b01
// Mask Pattern 0.
#define MASK_PATTERN_REFERENCE 0b000
#define COMBINED_FORMAT_INFORMATION 0b1111010110
#define FIXED_MASK_PATTERN 0b101010000010010

// See Table C.1
const unsigned short MASKED_FORMAT_INFORMATION = 0b111011111000100;

const char *MODULE_WHITE = "  ";
const char *MODULE_BLACK = "██";

const bool finderPattern[FINDER_PATTERN_SIZE_LENGTH]
                        [FINDER_PATTERN_SIZE_LENGTH] = {
                            {true, true, true, true, true, true, true},
                            {true, false, false, false, false, false, true},
                            {true, false, true, true, true, false, true},
                            {true, false, true, true, true, false, true},
                            {true, false, true, true, true, false, true},
                            {true, false, false, false, false, false, true},
                            {true, true, true, true, true, true, true}};

bool qrcode[VERSION_1_SIDE_LENGTH][VERSION_1_SIDE_LENGTH];

// Galois Field ---------------------------------------------------------------

// As defined by the QrCode standard for V1.
#define GF_SIZE 256
#define GF_PRIMITIVE_POLY 0b100011101  // x^8 + x^4 + x^3 + x^2 + 1

unsigned char gfExpLookupTable[GF_SIZE];
unsigned char gfLogLookupTable[GF_SIZE];

void initGfLookupTables() {
  unsigned short x = 1;  // Using short to detect overflows.

  // Generate the exponential table
  for (unsigned int i = 0; i < GF_SIZE - 1; i++) {
    gfExpLookupTable[i] = (unsigned char)x;
    gfLogLookupTable[(unsigned char)x] = i;

    // In GF(256) arithmetic, multiplying elements means performing polynomial
    // multiplication modulo the primitive polynomial.
    // Think of x as a representation of a polynomial. Shifting it left by one
    // effectively multiplies the polynomial by itself. Let's say
    // (hypothetically) that at some point in the loop, x is 5 (which represents
    // x^2 + 1). Then x << 1 will result in 10, which represents x^3 + x, which
    // is equivalent to x * (x^2 + 1) = x^3 + x.
    x = x << 1;
    if (x >= GF_SIZE) {
      x = x ^ GF_PRIMITIVE_POLY;
    }
  }

  // For expTable to be useful in multiplication, we need to make it circular
  // so that we can compute α^(i+j) easily when i+j ≥ 255
  gfExpLookupTable[GF_SIZE - 1] = gfExpLookupTable[0];  // α^255 = α^0 = 1
  gfLogLookupTable[0] = 0;  // Handle special case for logarithm of 0.
}

unsigned char gfAdd(unsigned char a, unsigned char b) { return a ^ b; }

unsigned char gfSub(unsigned char a, unsigned char b) { return gfAdd(a, b); }

unsigned char gfMul(unsigned char a, unsigned char b) {
  if (a == 0 || b == 0) {
    return 0;
  }

  // Remember: a * b = exp(log(a) + log(b))
  int logSum = gfLogLookupTable[a] + gfLogLookupTable[b];
  return gfExpLookupTable[logSum % (GF_SIZE - 1)];
}

unsigned char gfDiv(unsigned char a, unsigned char b) {
  if (b == 0) {
    fprintf(stderr, "Error: Division by zero in Galois Field\n");
    return 0;
  }

  if (a == 0) {
    return 0;
  }

  // Remember: a / b = exp(log(a) - log(b))
  int logDiff = gfLogLookupTable[a] - gfLogLookupTable[b];
  if (logDiff < 0) {
    logDiff += GF_SIZE - 1;
  }
  return gfExpLookupTable[(unsigned char)logDiff];
}
// Galois Field ---------------------------------------------------------------

// Reed-Solomon implementation ------------------------------------------------
unsigned char *createErrorCorrectionCodewords(
    const unsigned char *dataCodewords, size_t numDataCodewords,
    unsigned int numEcCodewords) {
  if (numEcCodewords == 0 || numDataCodewords == 0 || dataCodewords == NULL) {
    return NULL;
  }

  // See Table A.1
  unsigned char generatorPolynomialCoefficients[] = {
      1,                      // Coefficient of x^7 (always 1)
      gfExpLookupTable[87],   // Coefficient of x^6 (α^87)
      gfExpLookupTable[229],  // Coefficient of x^5 (α^229)
      gfExpLookupTable[146],  // Coefficient of x^4 (α^146)
      gfExpLookupTable[149],  // Coefficient of x^3 (α^149)
      gfExpLookupTable[238],  // Coefficient of x^2 (α^238)
      gfExpLookupTable[102],  // Coefficient of x^1 (α^102)
      gfExpLookupTable[21]    // Coefficient of x^0 (α^21)
  };

  unsigned char *messagePolynomial = (unsigned char *)calloc(
      numDataCodewords + numEcCodewords, sizeof(unsigned char));
  if (messagePolynomial == NULL) {
    return NULL;
  }
  memcpy(messagePolynomial, dataCodewords,
         numDataCodewords * sizeof(unsigned char));

  // Polynomial Division in GF(256).
  for (unsigned int i = 0; i < numDataCodewords; i++) {
    unsigned char factor = messagePolynomial[i];
    if (factor == 0) {
      continue;
    }

    for (unsigned int j = 0; j < numEcCodewords + 1; j++) {
      messagePolynomial[i + j] =
          gfSub(messagePolynomial[i + j],
                gfMul(factor, generatorPolynomialCoefficients[j]));
    }
  }

  unsigned char *ecCodewords =
      (unsigned char *)calloc(numEcCodewords, sizeof(unsigned char));
  if (ecCodewords == NULL) {
    free(messagePolynomial);
    return NULL;
  }

  // The remainder (error correction codewords) are the last numEcCodewords
  // bytes of messagePolynomial.
  memcpy(ecCodewords, messagePolynomial + numDataCodewords,
         numEcCodewords * sizeof(unsigned char));

  free(messagePolynomial);

  return ecCodewords;
}
// Reed-Solomon implementation ------------------------------------------------

/** Encodes an input string into bytes.
 *
 * These bytes include:
 * - The mode indicator (ENCODING_MODE_INDICATOR_BYTE)
 * - The count of characters in the orignal string (must be in 8bit)
 * - The actual string, encoded using utf8 bytes
 * - Terminator pattern (0000) if there is still space left
 * - Padding (if necessary)
 */
unsigned char *encodeString(const unsigned char *str, size_t codewordsSize) {
  size_t strLength = strlen(str);
  // 4 bits for the encoding mode, 8 bits for the string length -> 2 bytes out
  // of the available codewordsSize are not usable.
  if (strLength > codewordsSize - 2) {
    fprintf(stderr, "Input string too long: %d\n", strLength);
    return NULL;
  }
  unsigned char strLengthByte = (unsigned char)strLength;

  unsigned char *bitStream =
      (unsigned char *)calloc(codewordsSize, sizeof(unsigned char));
  bitStream[0] = ENCODING_MODE_INDICATOR_BYTE << 4;
  bitStream[0] |= strLengthByte >> 4;
  bitStream[1] = strLengthByte << 4;

  // Note that the memory is 0-ed already, so e.g. the terminator pattern does
  // not need to be applied manually.
  unsigned int bitStreamIndex = 1;
  for (unsigned int i = 0; i < strLength && bitStreamIndex < codewordsSize;
       i++) {
    unsigned char ch = str[i];
    bitStream[bitStreamIndex] |= ch >> 4;
    bitStream[++bitStreamIndex] = ch << 4;
  }

  // Add padding.
  bool lastPatternFirst = false;
  for (++bitStreamIndex; bitStreamIndex < codewordsSize; bitStreamIndex++) {
    bitStream[bitStreamIndex] = lastPatternFirst ? 0b00010001 : 0b11101100;
    lastPatternFirst = !lastPatternFirst;
  }

  return bitStream;
}

bool isHorizontalTimingPattern(unsigned int sideLength, int row, int column) {
  return row == FINDER_PATTERN_SIZE_LENGTH - 1 &&
         column >= FINDER_PATTERN_SIZE_LENGTH + 1 &&
         column <= sideLength - FINDER_PATTERN_SIZE_LENGTH - 2;
}

bool isVerticalTimingPattern(unsigned int sideLength, int row, int column) {
  return column == FINDER_PATTERN_SIZE_LENGTH - 1 &&
         row >= FINDER_PATTERN_SIZE_LENGTH + 1 &&
         row <= sideLength - FINDER_PATTERN_SIZE_LENGTH - 2;
}

bool isEncodingRegion(unsigned int sideLength, int row, int col) {
  if (row < 0 || row >= sideLength || col < 0 || col >= sideLength) {
    return false;
  }

  if (isHorizontalTimingPattern(sideLength, row, col) ||
      isVerticalTimingPattern(sideLength, row, col)) {
    return false;
  }

  // Bottom left finder pattern and format information collision.
  if (row >= sideLength - FINDER_PATTERN_SIZE_LENGTH - 1 &&
      col <= FINDER_PATTERN_SIZE_LENGTH + 1) {
    return false;
  }

  // Top right finder pattern and format information collision.
  if (row <= FINDER_PATTERN_SIZE_LENGTH + 1 &&
      col >= sideLength - FINDER_PATTERN_SIZE_LENGTH - 1) {
    return false;
  }

  // Top left finder pattern and format information collision.
  if (row <= FINDER_PATTERN_SIZE_LENGTH + 1 &&
      col <= FINDER_PATTERN_SIZE_LENGTH + 1) {
    return false;
  }

  return true;
}

void writeEncodedString(unsigned int sideLength,
                        const unsigned char *encodedStr,
                        size_t encodedStrLength) {
  /**
   * NOTE: this implementation takes some shortcuts under the assumption we are
   * generating a Version 1 QR Code. See page 10 of the specs for a clear
   * picture of what we are simplifying here.
   */

  int direction = -1;
  int row = sideLength - 1;
  int column =
      sideLength - 1;  // Always the rightmost column of the pair of modules.
  for (unsigned int i = 0; i < encodedStrLength; i++) {
    unsigned char word = encodedStr[i];

    if (!isEncodingRegion(sideLength, row, column)) {
      direction *= -1;
      row += direction;
      column -= 2;

      if (isVerticalTimingPattern(sideLength, row, column)) {
        column -= 1;
      }

      while (!isEncodingRegion(sideLength, row, column)) {
        // We hit a finder pattern.
        row += direction;
      }
    }

    for (unsigned int j = 0; j < 4; j++) {
      qrcode[row][column] = (word & (0b10000000 >> (2 * j))) != 0;
      qrcode[row][column - 1] = (word & (0b10000000 >> (2 * j + 1))) != 0;
      row += direction;

      if (isHorizontalTimingPattern(sideLength, row, column)) {
        row += direction;
      }
    }
  }
}

void writeFormatInformation(unsigned int sideLength) {
  // Placement 1.
  int bitIndex = 0;
  for (unsigned int i = 0; i <= 5; i++) {
    qrcode[i][FINDER_PATTERN_SIZE_LENGTH + 1] =
        (MASKED_FORMAT_INFORMATION & (1 << bitIndex++)) != 0;
  }

  // Row at index 6 is skipped as there we have the horizontal timing pattern.
  qrcode[7][FINDER_PATTERN_SIZE_LENGTH + 1] =
      (MASKED_FORMAT_INFORMATION & (1 << bitIndex++)) != 0;
  qrcode[8][FINDER_PATTERN_SIZE_LENGTH + 1] =
      (MASKED_FORMAT_INFORMATION & (1 << bitIndex++)) != 0;

  qrcode[8][FINDER_PATTERN_SIZE_LENGTH] =
      (MASKED_FORMAT_INFORMATION & (1 << bitIndex++)) != 0;
  // Column at index 6 is skipped as there we have the vertical timing pattern.
  for (int j = 5; j >= 0; j--) {
    qrcode[8][j] = (MASKED_FORMAT_INFORMATION & (1 << bitIndex++)) != 0;
  }

  // Placement 2.
  bitIndex = 0;
  for (unsigned int j = 0; j <= 7; j++) {
    qrcode[8][sideLength - 1 - j] =
        (MASKED_FORMAT_INFORMATION & (1 << bitIndex++)) != 0;
  }

  for (unsigned int i = sideLength - FINDER_PATTERN_SIZE_LENGTH; i < sideLength;
       i++) {
    qrcode[i][FINDER_PATTERN_SIZE_LENGTH + 1] =
        (MASKED_FORMAT_INFORMATION & (1 << bitIndex++)) != 0;
  }
}

void writeDarkModule(short version) { qrcode[4 * version + 9][8] = 1; }

void render(unsigned int sideLength, unsigned int quiteZoneSize) {
  int withQuiteZoneSize = sideLength + 2 * quiteZoneSize;
  for (unsigned int i = 0; i < withQuiteZoneSize; i++) {
    for (unsigned int j = 0; j < withQuiteZoneSize; j++) {
      if (i < quiteZoneSize || i >= withQuiteZoneSize - quiteZoneSize ||
          j < quiteZoneSize || j >= withQuiteZoneSize - quiteZoneSize) {
        printf("%s", MODULE_WHITE);
      } else {
        printf("%s", qrcode[i - quiteZoneSize][j - quiteZoneSize]
                         ? MODULE_BLACK
                         : MODULE_WHITE);
      }
    }
    printf("\n");
  }
}

void applyMaskPattern(unsigned int sideLength) {
  for (unsigned int row = 0; row < sideLength; row++) {
    for (unsigned int col = 0; col < sideLength; col++) {
      if (!isEncodingRegion(sideLength, row, col)) {
        continue;
      }

      if ((row + col) % 2 == 0) {
        qrcode[row][col] = !qrcode[row][col];
      }
    }
  }
}

void writeHorizontalTimingPattern(unsigned int row, unsigned int startColumn,
                                  unsigned int endColumn) {
  bool isBlack = true;
  for (unsigned int i = startColumn; i <= endColumn; i++) {
    qrcode[row][i] = isBlack;
    isBlack = !isBlack;
  }
}

void writeVerticalTimingPattern(unsigned int column, unsigned int startRow,
                                unsigned int endRow) {
  bool isBlack = true;
  for (unsigned int i = startRow; i <= endRow; i++) {
    qrcode[i][column] = isBlack;
    isBlack = !isBlack;
  }
}

void writeFinderPattern(unsigned int startRow, unsigned int startColumn) {
  for (unsigned int i = 0; i < FINDER_PATTERN_SIZE_LENGTH; i++) {
    for (unsigned int j = 0; j < FINDER_PATTERN_SIZE_LENGTH; j++) {
      qrcode[startRow + i][startColumn + j] = finderPattern[i][j];
    }
  }
}

void writeFinderPatterns(unsigned int sideLength) {
  writeFinderPattern(0, 0);
  writeFinderPattern(0, sideLength - FINDER_PATTERN_SIZE_LENGTH);
  writeFinderPattern(sideLength - FINDER_PATTERN_SIZE_LENGTH, 0);
}

int main(int argc, char *argv[]) {
  if (argc < 2) {
    printf("Supply a string to be encoded in the QR Code\n");
    return 1;
  }

  initGfLookupTables();

  writeFinderPatterns(VERSION_1_SIDE_LENGTH);

  writeHorizontalTimingPattern(
      FINDER_PATTERN_SIZE_LENGTH - 1, FINDER_PATTERN_SIZE_LENGTH + 1,
      VERSION_1_SIDE_LENGTH - FINDER_PATTERN_SIZE_LENGTH);
  writeVerticalTimingPattern(
      FINDER_PATTERN_SIZE_LENGTH - 1, FINDER_PATTERN_SIZE_LENGTH + 1,
      VERSION_1_SIDE_LENGTH - FINDER_PATTERN_SIZE_LENGTH);

  // Alignment patterns are present only in QR Code symbols of version 2 or
  // larger. Therefore, they are skipped here for now.

  size_t codewordsSize = 19;
  unsigned char *encodedString = encodeString(argv[1], codewordsSize);
  if (encodedString == NULL) {
    return 1;
  }

  unsigned char *errorCorrectionCodeWords =
      createErrorCorrectionCodewords(encodedString, codewordsSize, 7);
  if (errorCorrectionCodeWords == NULL) {
    free(encodedString);
    return 1;
  }

  size_t errorCorrectedEncodedStringLength = codewordsSize + 7;
  unsigned char *errorCorrectedEncodedString = (unsigned char *)calloc(
      errorCorrectedEncodedStringLength, sizeof(unsigned char));
  if (errorCorrectedEncodedString == NULL) {
    free(encodedString);
    free(errorCorrectionCodeWords);
    return 1;
  }

  memcpy(errorCorrectedEncodedString, encodedString, codewordsSize);
  memcpy(errorCorrectedEncodedString + codewordsSize, errorCorrectionCodeWords,
         7);

  writeEncodedString(VERSION_1_SIDE_LENGTH, errorCorrectedEncodedString,
                     errorCorrectedEncodedStringLength);

  free(encodedString);
  free(errorCorrectionCodeWords);
  free(errorCorrectedEncodedString);

  applyMaskPattern(VERSION_1_SIDE_LENGTH);

  writeFormatInformation(VERSION_1_SIDE_LENGTH);
  writeDarkModule(VERSION);

  render(VERSION_1_SIDE_LENGTH, QUIET_ZONE_SIZE);
  return 0;
}
