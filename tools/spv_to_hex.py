#!/usr/bin/env python3
import sys
import os

def spv_to_hex(spv_path, header_path, var_name):
    if not os.path.exists(spv_path):
        print(f"Error: Input file {spv_path} not found.")
        sys.exit(1)
        
    with open(spv_path, "rb") as f:
        data = f.read()
        
    # Ensure size is a multiple of 4 bytes
    if len(data) % 4 != 0:
        print("Warning: SPIR-V binary size is not a multiple of 4 bytes. Padding with zeros.")
        data += b"\x00" * (4 - (len(data) % 4))
        
    # Convert to 32-bit integers
    words = []
    for i in range(0, len(data), 4):
        word = int.from_bytes(data[i:i+4], byteorder="little")
        words.append(word)
        
    # Write C++ header
    os.makedirs(os.path.dirname(header_path), exist_ok=True)
    with open(header_path, "w") as f:
        f.write("#pragma once\n")
        f.write("#include <cstdint>\n\n")
        f.write(f"// Automatically generated from {os.path.basename(spv_path)}. Do not edit.\n")
        f.write(f"inline const uint32_t {var_name}[] = {{\n")
        
        # Write words in rows of 8 for readability
        for i in range(0, len(words), 8):
            row = words[i:i+8]
            hex_str = ", ".join(f"0x{w:08x}" for w in row)
            comma = "," if i + 8 < len(words) else ""
            f.write(f"    {hex_str}{comma}\n")
            
        f.write("};\n")
        f.write(f"inline const size_t {var_name}_size = sizeof({var_name});\n")

if __name__ == "__main__":
    if len(sys.argv) != 4:
        print("Usage: spv_to_hex.py <input.spv> <output.h> <variable_name>")
        sys.exit(1)
        
    spv_to_hex(sys.argv[1], sys.argv[2], sys.argv[3])
