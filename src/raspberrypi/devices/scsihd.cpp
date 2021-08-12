//---------------------------------------------------------------------------
//
//	SCSI Target Emulator RaSCSI (*^..^*)
//	for Raspberry Pi
//
//	Copyright (C) 2001-2006 ＰＩ．(ytanaka@ipc-tokai.or.jp)
//	Copyright (C) 2014-2020 GIMONS
//	Copyright (C) akuker
//
//	Licensed under the BSD 3-Clause License. 
//	See LICENSE file in the project root folder.
//
//	[ SCSI hard disk ]
//
//---------------------------------------------------------------------------
#include "scsihd.h"
#include "xm6.h"
#include "fileio.h"
#include "exceptions.h"

#include <iostream>

//===========================================================================
//
//	SCSI Hard Disk
//
//===========================================================================

//---------------------------------------------------------------------------
//
//	Constructor
//
//---------------------------------------------------------------------------
SCSIHD::SCSIHD(bool removable) : Disk(removable ? "SCRM" : "SCHD", removable)
{
	SetProtected(true);
}

//---------------------------------------------------------------------------
//
//	Reset
//
//---------------------------------------------------------------------------
void SCSIHD::Reset()
{
	// Unlock and release attention
	SetLocked(false);
	SetAttn(false);

	// No reset, clear code
	SetReset(false);
	SetStatusCode(STATUS_NOERROR);
}

//---------------------------------------------------------------------------
//
//	Open
//
//---------------------------------------------------------------------------
void SCSIHD::Open(const Filepath& path)
{
	ASSERT(!IsReady());

	// read open required
	Fileio fio;
	if (!fio.Open(path, Fileio::ReadOnly)) {
		throw ioexception("Can't open hard disk file read-only");
	}

	// Get file size
	off64_t size = fio.GetFileSize();
	fio.Close();

	// Must be 512 bytes
	if (size & 0x1ff) {
		throw ioexception("File size must be a multiple of 512 bytes");
	}

    // 2TB according to xm6i
    // There is a similar one in wxw/wxw_cfg.cpp
	// Bigger files/drives require READ/WRITE(16) to be implemented
	if (size > 2LL * 1024 * 1024 * 1024 * 1024) {
		throw ioexception("File size must not exceed 2 TB");
	}

	// sector size and number of blocks
	disk.size = 9;
	disk.blocks = (DWORD)(size >> 9);

	// Set the default product name based on the drive capacity
	// TODO Use C++20 string formatting asap
	int capacity = disk.blocks >> 11;
	char product[17];
	if (capacity < 300) {
		sprintf(product, "PRODRIVE LPS%dS", capacity);
	}
	else if (capacity < 600) {
		sprintf(product, "MAVERICK%dS", capacity);
	}
	else if (capacity < 800) {
		sprintf(product, "LIGHTNING%dS", capacity);
	}
	else if (capacity < 1000) {
		sprintf(product, "TRAILBRAZER%dS", capacity);
	}
	else if (capacity < 2000) {
		sprintf(product, "FIREBALL%dS", capacity);
	}
	else {
		sprintf(product, "FBSE%d.%dS", capacity / 1000, (capacity % 1000) / 100);
	}
	SetProduct(product, false);

	Disk::Open(path);
	FileSupport::SetPath(path);
}

//---------------------------------------------------------------------------
//
//	INQUIRY
//
//---------------------------------------------------------------------------
int SCSIHD:: Inquiry(const DWORD *cdb, BYTE *buf, DWORD major, DWORD minor)
{
	ASSERT(cdb);
	ASSERT(buf);
	ASSERT(cdb[0] == 0x12);

	// EVPD check
	if (cdb[1] & 0x01) {
		SetStatusCode(STATUS_INVALIDCDB);
		return 0;
	}

	// Ready check (Error if no image file)
	if (!IsReady()) {
		SetStatusCode(STATUS_NOTREADY);
		return 0;
	}

	// Basic data
	// buf[0] ... Direct Access Device
	// buf[2] ... SCSI-2 compliant command system
	// buf[3] ... SCSI-2 compliant Inquiry response
	// buf[4] ... Inquiry additional data
	memset(buf, 0, 8);

	// SCSI-2 p.104 4.4.3 Incorrect logical unit handling
	if (((cdb[1] >> 5) & 0x07) != GetLun()) {
		buf[0] = 0x7f;
	}

	buf[2] = 0x02;
	buf[3] = 0x02;
	buf[4] = 122 + 3;	// Value close to real HDD

	// Padded vendor, product, revision
	string name;
	GetPaddedName(name);
	memcpy(&buf[8], name.c_str(), 28);

	// Size of data that can be returned
	int size = (buf[4] + 5);

	// Limit if the other buffer is small
	if (size > (int)cdb[4]) {
		size = (int)cdb[4];
	}

	//  Success
	SetStatusCode(STATUS_NOERROR);
	return size;
}

//---------------------------------------------------------------------------
//
//	MODE SELECT
//	*Not affected by disk.code
//
//---------------------------------------------------------------------------
BOOL SCSIHD::ModeSelect(const DWORD *cdb, const BYTE *buf, int length)
{
	BYTE page;
	int size;

	ASSERT(buf);
	ASSERT(length >= 0);

	// PF
	if (cdb[1] & 0x10) {
		// Mode Parameter header
		if (length >= 12) {
			// Check the block length bytes
			size = 1 << disk.size;
			if (buf[9] != (BYTE)(size >> 16) ||
				buf[10] != (BYTE)(size >> 8) ||
				buf[11] != (BYTE)size) {
				// currently does not allow changing sector length
				SetStatusCode(STATUS_INVALIDPRM);
				return FALSE;
			}
			buf += 12;
			length -= 12;
		}

		// Parsing the page
		while (length > 0) {
			// Get page
			page = buf[0];

			switch (page) {
				// format device
				case 0x03:
					// check the number of bytes in the physical sector
					size = 1 << disk.size;
					if (buf[0xc] != (BYTE)(size >> 8) ||
						buf[0xd] != (BYTE)size) {
						// currently does not allow changing sector length
						SetStatusCode(STATUS_INVALIDPRM);
						return FALSE;
					}
					break;

                // CD-ROM Parameters
                // According to the SONY CDU-541 manual, Page code 8 is supposed
                // to set the Logical Block Adress Format, as well as the
                // inactivity timer multiplier
                case 0x08:
                    // Debug code for Issue #2:
                    //     https://github.com/akuker/RASCSI/issues/2
                    LOGWARN("[Unhandled page code] Received mode page code 8 with total length %d\n     ", length);
                    for (int i = 0; i<length; i++)
                    {
                        printf("%02X ", buf[i]);
                    }
                    printf("\n");
                    break;
				// Other page
				default:
                    printf("Unknown Mode Select page code received: %02X\n",page);
					break;
			}

			// Advance to the next page
			size = buf[1] + 2;
			length -= size;
			buf += size;
		}
	}

	// Do not generate an error for the time being (MINIX)
	SetStatusCode(STATUS_NOERROR);

	return TRUE;
}
