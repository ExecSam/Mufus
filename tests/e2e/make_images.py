# Creates the test images used by the Mufus end-to-end tests:
# - dd.img:   a 32 MB raw disk image (MBR with a boot marker + pseudo random data)
# - efi.iso:  a UEFI bootable ISO (EFI/BOOT/BOOTX64.EFI) with 48 MB of data files
# - efi.iso.manifest: the list of files in the ISO, with their SHA-256
import hashlib, io, os, random, struct, sys

out = sys.argv[1]
os.makedirs(out, exist_ok=True)
rng = random.Random(0x4D554655)  # "MUFU"

# DD image
size = 32 * 1024 * 1024
data = bytearray(rng.randbytes(size))
mbr = bytearray(512)
mbr[0:3] = b"\xEB\x58\x90"
# One FAT32 (LBA) partition entry starting at sector 2048
entry = struct.pack("<B3sB3sII", 0x80, b"\x20\x21\x00", 0x0C, b"\xFE\xFF\xFF", 2048, (size // 512) - 2048)
mbr[446:462] = entry
mbr[440:444] = struct.pack("<I", 0x4D554655)
mbr[510:512] = b"\x55\xAA"
data[0:512] = mbr
with open(os.path.join(out, "dd.img"), "wb") as f:
	f.write(data)
print("dd.img", hashlib.sha256(data).hexdigest())

# EFI ISO
import pycdlib
iso = pycdlib.PyCdlib()
iso.new(interchange_level=3, joliet=3, vol_ident="MUFUS_TEST")
manifest = []

def add_dir(path):
	iso.add_directory(iso_path="/" + path.upper().replace("/", "/"), joliet_path="/" + path)

def add_file(path, content):
	iso.add_fp(io.BytesIO(content), len(content), iso_path="/" + path.upper() + ";1", joliet_path="/" + path)
	manifest.append((path, hashlib.sha256(content).hexdigest()))

add_dir("EFI")
add_dir("EFI/BOOT")
# A minimal (unsigned) PE stub is all Rufus needs to detect a UEFI bootloader
pe = bytearray(4096)
pe[0:2] = b"MZ"
struct.pack_into("<I", pe, 0x3C, 0x80)
pe[0x80:0x84] = b"PE\x00\x00"
struct.pack_into("<H", pe, 0x84, 0x8664)
add_file("EFI/BOOT/BOOTX64.EFI", bytes(pe))
add_file("README.TXT", b"Mufus multi-drive end-to-end test image\r\n")
add_dir("DATA")
for i in range(48):
	add_file(f"DATA/FILE{i:03d}.BIN", rng.randbytes(1024 * 1024))
iso.write(os.path.join(out, "efi.iso"))
iso.close()
with open(os.path.join(out, "efi.iso.manifest"), "w") as f:
	for path, h in manifest:
		f.write(f"{path}\t{h}\n")
print("efi.iso", len(manifest), "files")
