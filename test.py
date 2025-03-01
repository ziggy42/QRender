# Copyright 2025 Google LLC
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

import subprocess
import random
import string
from PIL import Image
import pyzbar.pyzbar as pyzbar

MODULE_WHITE = "  "
MODULE_BLACK = "██"

N_ITERATIONS = 100

# TODO: why including these other sets of character cause multiple tests to fail, even though
# qr codes generated for those screens can be scanned (at least with Google Camera)?
ALL_PRINTABLE_CHARACTERS = ''.join([
  string.printable,                                       # ASCII printable characters
  # ''.join(chr(cp) for cp in range(0x00A1, 0x00FF + 1)),   # Latin-1 Supplement
  # ''.join(chr(cp) for cp in range(0x0100, 0x017F + 1)),   # Latin Extended-A
  # ''.join(chr(cp) for cp in range(0x0370, 0x03FF + 1)),   # Greek and Coptic
  # ''.join(chr(cp) for cp in range(0x0400, 0x04FF + 1)),   # Cyrillic
  # ''.join(chr(cp) for cp in range(0x2200, 0x22FF + 1)),   # Mathematical Operators
  ''.join(chr(cp) for cp in range(0x1F600, 0x1F64F + 1)), # Emojis
  # ''.join(chr(cp) for cp in range(0x3000, 0x303F + 1)),   # CJK Symbols and Punctuation
  # ''.join(chr(cp) for cp in range(0x4E00, 0x4FFF + 1))    # Common CJK Unified Ideographs (subset)
])


def generate_random_string(max_bytes = 17):
  result = ""
  current_bytes = 0

  # We pick 4 as minimum since that's the maximum number of bytes a single
  # unicode characters can take.
  random_length = random.randint(4, max_bytes)
  while current_bytes < random_length:
    char = random.choice(ALL_PRINTABLE_CHARACTERS)
    new_result = result + char

    bytes_length = len(new_result.encode('utf-8'))

    # If adding this character would exceed random_length, break the loop
    # unless we haven't reached min_bytes yet
    if bytes_length > random_length:
      break

    # Otherwise, update the result and byte count
    result = new_result
    current_bytes = bytes_length

  return result


def compile():
  subprocess.run(["gcc", "qrender.c", "-o", "qrender"], check=True)


def run_qrender(input_string):
  result = subprocess.run(["./qrender", input_string], capture_output=True, text=True, check=True)
  return result.stdout


def get_qr_image_from_text(qr_text):
  lines = qr_text.split('\n')

  # Since each module is 2 characters wide, we need to adjust the width.
  width = max(len(line) for line in lines) // 2
  height = len(lines)

  # Create a new blank image with white background.
  scale = 10  # Scale factor for better resolution.
  img = Image.new('1', (width * scale, height * scale), 1)
  pixels = img.load()

  # Fill in black pixels where there are black modules.
  for y, line in enumerate(lines):
    for x in range(0, len(line), 2):  # Step by 2 characters.
      if x + 1 < len(line):
        chars = line[x:x + 2]
        if chars == MODULE_BLACK:
          # Fill a scaled block of pixels.
          for sy in range(scale):
            for sx in range(scale):
              pixels[(x // 2) * scale + sx, y * scale + sy] = 0  # Black pixel.
  return img


def test_qrender(input_text):
  qr_text = run_qrender(input_text)
  qr_image = get_qr_image_from_text(qr_text)

  decoded_objects = pyzbar.decode(qr_image)
  decoded_text = decoded_objects[0].data.decode('utf-8')

  if decoded_text == input_text:
    print(f"✅ Decoded: \"{decoded_text}\"")
  else:
    print(f"⛔ Expected: \"{input_text}\" Actual: \"{decoded_text}\"")


if __name__ == "__main__":
  compile()
  for _ in range(N_ITERATIONS):
    test_qrender(generate_random_string())
