/*
 * Rufus: The Reliable USB Formatting Utility
 * Virtual Disk Handling functions
 * Copyright © 2013-2014 Pete Batard <pete@akeo.ie>
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <windows.h>
#include <stdlib.h>
#include <io.h>
#include <rpc.h>
#include <time.h>

#include "rufus.h"
#include "msapi_utf8.h"
#include "drive.h"
#include "registry.h"
#include "bled/bled.h"

#if defined(_MSC_VER)
#define bswap_uint64 _byteswap_uint64
#define bswap_uint32 _byteswap_ulong
#define bswap_uint16 _byteswap_ushort
#else
#define bswap_uint64 __builtin_bswap64
#define bswap_uint32 __builtin_bswap32
#define bswap_uint16 __builtin_bswap16
#endif

#define VHD_FOOTER_COOKIE					{ 'c', 'o', 'n', 'e', 'c', 't', 'i', 'x' }

#define VHD_FOOTER_FEATURES_NONE			0x00000000
#define VHD_FOOTER_FEATURES_TEMPORARY		0x00000001
#define VHD_FOOTER_FEATURES_RESERVED		0x00000002

#define VHD_FOOTER_FILE_FORMAT_V1_0			0x00010000

#define VHD_FOOTER_DATA_OFFSET_FIXED_DISK	0xFFFFFFFFFFFFFFFFULL

#define VHD_FOOTER_CREATOR_HOST_OS_WINDOWS	{ 'W', 'i', '2', 'k' }
#define VHD_FOOTER_CREATOR_HOST_OS_MAC		{ 'M', 'a', 'c', ' ' }

#define VHD_FOOTER_TYPE_FIXED_HARD_DISK		0x00000002
#define VHD_FOOTER_TYPE_DYNAMIC_HARD_DISK	0x00000003
#define VHD_FOOTER_TYPE_DIFFER_HARD_DISK	0x00000004

#define SECONDS_SINCE_JAN_1ST_2000			946684800

/*
 * VHD Fixed HD footer (Big Endian)
 * http://download.microsoft.com/download/f/f/e/ffef50a5-07dd-4cf8-aaa3-442c0673a029/Virtual%20Hard%20Disk%20Format%20Spec_10_18_06.doc
 * NB: If a dymamic implementation is needed, check the GPL v3 compatible C++ implementation from:
 * https://sourceforge.net/p/urbackup/backend/ci/master/tree/fsimageplugin/
 */
#pragma pack(push, 1)
typedef struct vhd_footer {
	char		cookie[8];
	uint32_t	features;
	uint32_t	file_format_version;
	uint64_t	data_offset;
	uint32_t	timestamp;
	char		creator_app[4];
	uint32_t	creator_version;
	char		creator_host_os[4];
	uint64_t	original_size;
	uint64_t	current_size;
	union {
		uint32_t	geometry;
		struct {
			uint16_t	cylinders;
			uint8_t		heads;
			uint8_t		sectors;
		} chs;
	} disk_geometry;
	uint32_t	disk_type;
	uint32_t	checksum;
	uuid_t		unique_id;
	uint8_t		saved_state;
	uint8_t		reserved[427];
} vhd_footer;
#pragma pack(pop)

// WIM API Prototypes
#define WIM_GENERIC_READ	GENERIC_READ
#define WIM_OPEN_EXISTING	OPEN_EXISTING
PF_TYPE_DECL(WINAPI, HANDLE, WIMCreateFile, (PWSTR, DWORD, DWORD, DWORD, DWORD, PDWORD));
PF_TYPE_DECL(WINAPI, BOOL, WIMSetTemporaryPath, (HANDLE, PWSTR));
PF_TYPE_DECL(WINAPI, HANDLE, WIMLoadImage, (HANDLE, DWORD));
PF_TYPE_DECL(WINAPI, BOOL, WIMExtractImagePath, (HANDLE, PWSTR, PWSTR, DWORD));
PF_TYPE_DECL(WINAPI, BOOL, WIMCloseHandle, (HANDLE));
PF_TYPE_DECL(RPC_ENTRY, RPC_STATUS, UuidCreate, (UUID __RPC_FAR*));

static BOOL has_wimgapi = FALSE, has_7z = FALSE;
static char sevenzip_path[MAX_PATH];
static const char conectix_str[] = VHD_FOOTER_COOKIE;

static BOOL Get7ZipPath(void)
{
	if ( (GetRegistryKeyStr(REGKEY_HKCU, "7-Zip\\Path", sevenzip_path, sizeof(sevenzip_path)))
	  || (GetRegistryKeyStr(REGKEY_HKLM, "7-Zip\\Path", sevenzip_path, sizeof(sevenzip_path))) ) {
		safe_strcat(sevenzip_path, sizeof(sevenzip_path), "\\7z.exe");
		return (_access(sevenzip_path, 0) != -1);
	}
	return FALSE;
}

BOOL AppendVHDFooter(const char* vhd_path)
{
	const char creator_os[4] = VHD_FOOTER_CREATOR_HOST_OS_WINDOWS;
	const char creator_app[4] = { 'r', 'u', 'f', 'u' };
	BOOL r = FALSE;
	DWORD size;
	LARGE_INTEGER li;
	HANDLE handle = INVALID_HANDLE_VALUE;
	vhd_footer* footer = NULL;
	uint64_t totalSectors;
	uint16_t cylinders = 0;
	uint8_t heads, sectorsPerTrack;
	uint32_t cylinderTimesHeads;
	uint32_t checksum;
	size_t i;

	PF_INIT(UuidCreate, Rpcrt4);
	handle = CreateFileU(vhd_path, GENERIC_WRITE, FILE_SHARE_WRITE, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	li.QuadPart = 0;
	if ((handle == INVALID_HANDLE_VALUE) || (!SetFilePointerEx(handle, li, &li, FILE_END))) {
		uprintf("Could not open image '%s': %s", vhd_path, WindowsErrorString());
		goto out;
	}
	footer = (vhd_footer*)calloc(1, sizeof(vhd_footer));
	if (footer == NULL) {
		uprintf("Could not allocate VHD footer");
		goto out;
	}

	memcpy(footer->cookie, conectix_str, sizeof(footer->cookie));
	footer->features = bswap_uint32(VHD_FOOTER_FEATURES_RESERVED);
	footer->file_format_version = bswap_uint32(VHD_FOOTER_FILE_FORMAT_V1_0);
	footer->data_offset = bswap_uint64(VHD_FOOTER_DATA_OFFSET_FIXED_DISK);
	footer->timestamp = bswap_uint32(_time32(NULL) - SECONDS_SINCE_JAN_1ST_2000);
	memcpy(footer->creator_app, creator_app, sizeof(creator_app));
	footer->creator_version = bswap_uint32((rufus_version[0]<<16)|rufus_version[1]);
	memcpy(footer->creator_host_os, creator_os, sizeof(creator_os));
	footer->original_size = bswap_uint64(li.QuadPart);
	footer->current_size = footer->original_size;
	footer->disk_type = bswap_uint32(VHD_FOOTER_TYPE_FIXED_HARD_DISK);
	if ((pfUuidCreate == NULL) || (pfUuidCreate(&footer->unique_id) != RPC_S_OK))
		uprintf("Warning: could not set VHD UUID");

	// Compute CHS, as per the VHD specs
	totalSectors = li.QuadPart / 512;
	if (totalSectors > 65535 * 16 * 255) {
		totalSectors = 65535 * 16 * 255;
	}

	if (totalSectors >= 65535 * 16 * 63) {
		sectorsPerTrack = 255;
		heads = 16;
		cylinderTimesHeads = (uint32_t)(totalSectors / sectorsPerTrack);
	} else {
		sectorsPerTrack = 17; 
		cylinderTimesHeads = (uint32_t)(totalSectors / sectorsPerTrack);

		heads = (cylinderTimesHeads + 1023) / 1024;

		if (heads < 4) {
			heads = 4;
		}
		if (cylinderTimesHeads >= ((uint32_t)heads * 1024) || heads > 16) {
			sectorsPerTrack = 31;
			heads = 16;
			cylinderTimesHeads = (uint32_t)(totalSectors / sectorsPerTrack);
		}
		if (cylinderTimesHeads >= ((uint32_t)heads * 1024)) {
			sectorsPerTrack = 63;
			heads = 16;
			cylinderTimesHeads = (uint32_t)(totalSectors / sectorsPerTrack);
		}
	}
	cylinders = cylinderTimesHeads / heads;
	footer->disk_geometry.chs.cylinders = bswap_uint16(cylinders);
	footer->disk_geometry.chs.heads = heads;
	footer->disk_geometry.chs.sectors = sectorsPerTrack;

	// Compute the VHD footer checksum
	for (checksum=0, i=0; i<sizeof(vhd_footer); i++)
		checksum += ((uint8_t*)footer)[i];
	footer->checksum = bswap_uint32(~checksum);

	if (!WriteFile(handle, footer, sizeof(vhd_footer), &size, NULL) || (size != sizeof(vhd_footer))) {
		uprintf("Could not write VHD footer: %s", WindowsErrorString());
		goto out;
	}
	r = TRUE;
	
out:
	safe_free(footer);
	safe_closehandle(handle);
	return r;
}

typedef struct {
	const char* ext;
	bled_compression_type type;
} comp_assoc;

static comp_assoc blah[] = {
	{ ".xz", BLED_COMPRESSION_XZ },
	{ ".gz", BLED_COMPRESSION_GZIP },
	{ ".lzma", BLED_COMPRESSION_LZMA },
	{ ".bz2", BLED_COMPRESSION_BZIP2 },
	{ ".Z", BLED_COMPRESSION_LZW },
};

// For now we consider that an image that matches a known extension is bootable
// TODO: uncompress header and check for bootable flag
BOOL IsCompressedBootableImage(const char* path)
{
	char* p;
	int i;

	iso_report.compression_type = BLED_COMPRESSION_NONE;
	for (p = (char*)&path[strlen(path)-1]; (*p != '.') && (p != path); p--);

	if (p == path)
		return FALSE;

	for (i = 0; i<ARRAYSIZE(blah); i++) {
		if (strcmp(p, blah[i].ext) == 0) {
			iso_report.compression_type = blah[i].type;
			return TRUE;
		}
	}

	return FALSE;
}


BOOL IsHDImage(const char* path)
{
	HANDLE handle = INVALID_HANDLE_VALUE;
	LARGE_INTEGER liImageSize;
	vhd_footer* footer = NULL;
	DWORD size;
	size_t i;
	uint32_t checksum, old_checksum;
	LARGE_INTEGER ptr;

	handle = CreateFileU(path, GENERIC_READ, FILE_SHARE_READ, NULL,
		OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (handle == INVALID_HANDLE_VALUE) {
		uprintf("Could not open image '%s'", path);
		goto out;
	}

	iso_report.is_bootable_img = IsCompressedBootableImage(path);
	if (iso_report.compression_type == BLED_COMPRESSION_NONE)
		iso_report.is_bootable_img = AnalyzeMBR(handle, "Image");

	if (!GetFileSizeEx(handle, &liImageSize)) {
		uprintf("Could not get image size: %s", WindowsErrorString());
		goto out;
	}
	iso_report.projected_size = (uint64_t)liImageSize.QuadPart;

	size = sizeof(vhd_footer);
	if ((iso_report.compression_type == BLED_COMPRESSION_NONE) && (iso_report.projected_size >= (512 + size))) {
		footer = (vhd_footer*)malloc(size);
		ptr.QuadPart = iso_report.projected_size - size;
		if ( (footer == NULL) || (!SetFilePointerEx(handle, ptr, NULL, FILE_BEGIN)) ||
			 (!ReadFile(handle, footer, size, &size, NULL)) || (size != sizeof(vhd_footer)) ) {
			uprintf("Could not read VHD footer");
			goto out;
		}
		if (memcmp(footer->cookie, conectix_str, sizeof(footer->cookie)) == 0) {
			iso_report.projected_size -= sizeof(vhd_footer);
			if ( (bswap_uint32(footer->file_format_version) != VHD_FOOTER_FILE_FORMAT_V1_0)
			  || (bswap_uint32(footer->disk_type) != VHD_FOOTER_TYPE_FIXED_HARD_DISK)) {
				uprintf("Unsupported type of VHD image");
				iso_report.is_bootable_img = FALSE;
				goto out;
			}
			// Might as well validate the checksum while we're at it
			old_checksum = bswap_uint32(footer->checksum);
			footer->checksum = 0;
			for (checksum=0, i=0; i<sizeof(vhd_footer); i++)
				checksum += ((uint8_t*)footer)[i];
			checksum = ~checksum;
			if (checksum != old_checksum)
				uprintf("Warning: VHD footer seems corrupted (checksum: %04X, expected: %04X)", old_checksum, checksum);
			// Need to remove the footer from our payload
			uprintf("Image is a Fixed Hard Disk VHD file");
			iso_report.is_vhd = TRUE;
		}
	}

out:
	safe_free(footer);
	safe_closehandle(handle);
	return iso_report.is_bootable_img;
}

// Find out if we have any way to extract WIM files on this platform
BOOL WimExtractCheck(void)
{
	PF_INIT(WIMCreateFile, Wimgapi);
	PF_INIT(WIMSetTemporaryPath, Wimgapi);
	PF_INIT(WIMLoadImage, Wimgapi);
	PF_INIT(WIMExtractImagePath, Wimgapi);
	PF_INIT(WIMCloseHandle, Wimgapi);

	has_wimgapi = (pfWIMCreateFile && pfWIMSetTemporaryPath && pfWIMLoadImage && pfWIMExtractImagePath && pfWIMCloseHandle);
	has_7z = Get7ZipPath();

	uprintf("WIM extraction method(s) supported: %s%s%s\n", has_7z?"7z":(has_wimgapi?"":"NONE"),
		(has_wimgapi && has_7z)?", ":"", has_wimgapi?"wimgapi.dll":"");
	return (has_wimgapi || has_7z);
}


// Extract a file from a WIM image using wimgapi.dll (Windows 7 or later)
// NB: if you want progress from a WIM callback, you must run the WIM API call in its own thread
// (which we don't do here) as it won't work otherwise. Thanks go to Erwan for figuring this out!
static BOOL WimExtractFile_API(const char* image, int index, const char* src, const char* dst)
{
	BOOL r = FALSE;
	DWORD dw = 0;
	HANDLE hWim = NULL;
	HANDLE hImage = NULL;
	wchar_t wtemp[MAX_PATH] = {0};
	wchar_t* wimage = utf8_to_wchar(image);
	wchar_t* wsrc = utf8_to_wchar(src);
	wchar_t* wdst = utf8_to_wchar(dst);

	PF_INIT_OR_OUT(WIMCreateFile, Wimgapi);
	PF_INIT_OR_OUT(WIMSetTemporaryPath, Wimgapi);
	PF_INIT_OR_OUT(WIMLoadImage, Wimgapi);
	PF_INIT_OR_OUT(WIMExtractImagePath, Wimgapi);
	PF_INIT_OR_OUT(WIMCloseHandle, Wimgapi);

	uprintf("Opening: %s:[%d] (API)\n", image, index);
	if (GetTempPathW(ARRAYSIZE(wtemp), wtemp) == 0) {
		uprintf("  Could not fetch temp path: %s\n", WindowsErrorString());
		goto out;
	}

	hWim = pfWIMCreateFile(wimage, WIM_GENERIC_READ, WIM_OPEN_EXISTING, 0, 0, &dw);
	if (hWim == NULL) {
		uprintf("  Could not access image: %s\n", WindowsErrorString());
		goto out;
	}

	if (!pfWIMSetTemporaryPath(hWim, wtemp)) {
		uprintf("  Could not set temp path: %s\n", WindowsErrorString());
		goto out;
	}

	hImage = pfWIMLoadImage(hWim, (DWORD)index);
	if (hImage == NULL) {
		uprintf("  Could not set index: %s\n", WindowsErrorString());
		goto out;
	}

	uprintf("Extracting: %s (From %s)\n", dst, src);
	if (!pfWIMExtractImagePath(hImage, wsrc, wdst, 0)) {
		uprintf("  Could not extract file: %s\n", WindowsErrorString());
		goto out;
	}
	r = TRUE;
	UpdateProgress(OP_FINALIZE, -1.0f);

out:
	if ((hImage != NULL) || (hWim != NULL)) {
		uprintf("Closing: %s\n", image);
	}
	if (hImage != NULL) pfWIMCloseHandle(hImage);
	if (hWim != NULL) pfWIMCloseHandle(hWim);
	safe_free(wimage);
	safe_free(wsrc);
	safe_free(wdst);
	return r;
}

// Extract a file from a WIM image using 7-Zip
static BOOL WimExtractFile_7z(const char* image, int index, const char* src, const char* dst)
{
	size_t i;
	STARTUPINFOA si = {0};
	PROCESS_INFORMATION pi = {0};
	char cmdline[MAX_PATH];
	char tmpdst[MAX_PATH];

	uprintf("Opening: %s:[%d] (7-Zip)\n", image, index);
	safe_strcpy(tmpdst, sizeof(tmpdst), dst);
	for (i=safe_strlen(tmpdst); i>0; i--) {
		if (tmpdst[i] == '\\')
			break;
	}
	tmpdst[i] = 0;

	si.cb = sizeof(si);
	safe_sprintf(cmdline, sizeof(cmdline), "7z -y e \"%s\" %d\\%s", image, index, src);
	uprintf("Extracting: %s (From %s)\n", dst, src);
	if (!CreateProcessU(sevenzip_path, cmdline, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, tmpdst, &si, &pi)) {
		uprintf("  Could not launch 7z.exe: %s\n", WindowsErrorString());
		return FALSE;
	}
	WaitForSingleObject(pi.hProcess, INFINITE);
	UpdateProgress(OP_FINALIZE, -1.0f);
	CloseHandle(pi.hProcess);
	CloseHandle(pi.hThread);

	safe_strcat(tmpdst, sizeof(tmpdst), "\\bootmgfw.efi");
	if (_access(tmpdst, 0) == -1) {
		uprintf("  7z.exe did not extract %s\n", tmpdst);
		return FALSE;
	}
	// coverity[toctou]
	if (rename(tmpdst, dst) != 0) {
		uprintf("  Could not rename %s to %s\n", tmpdst, dst);
		return FALSE;
	}

	return TRUE;
}

// Extract a file from a WIM image
BOOL WimExtractFile(const char* image, int index, const char* src, const char* dst)
{
	if ((!has_wimgapi) && (!has_7z) && (!WimExtractCheck()))
		return FALSE;
	if ((image == NULL) || (src == NULL) || (dst == NULL))
		return FALSE;

	// Prefer 7-Zip as, unsurprisingly, it's faster than the Microsoft way,
	// but allow fallback if 7-Zip doesn't succeed
	return ( (has_7z && WimExtractFile_7z(image, index, src, dst))
		  || (has_wimgapi && WimExtractFile_API(image, index, src, dst)) );
}
