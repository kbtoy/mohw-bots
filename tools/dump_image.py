"""Write a PE's in-memory image (sections at their virtual addresses) to a flat file, for tools/smoketest/imagetest.

Usage: python -I dump_image.py <lanmohw.exe> <out.bin>
"""
import sys

import pefile

pe = pefile.PE(sys.argv[1])
image = pe.get_memory_mapped_image()
open(sys.argv[2], "wb").write(image)
print(f"wrote {len(image):#x} bytes (SizeOfImage {pe.OPTIONAL_HEADER.SizeOfImage:#x})")
