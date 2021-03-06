/*
**
**
**
** This file's purpose is to emulate the inner workings of a
** GameBoy Game Pak cartrige.
**
** Original code is by Mark McGough. MadManMarkAu@hotmail.com
**
**
*/

#include "commonIncludes.h"
#include <windows.h>
#include "goombasav/goombasav.h"
#include "NRagePluginV2.h"
#include "PakIO.h"
#include "GBCart.h"

void ClearData(BYTE *Data, int Length);

bool ReadCartNorm(LPGBCART Cart, WORD dwAddress, BYTE *Data); // For all non-MBC carts; fixed 0x8000 ROM; fixed, optional 0x2000 RAM
bool WriteCartNorm(LPGBCART Cart, WORD dwAddress, BYTE *Data);
bool ReadCartMBC1(LPGBCART Cart, WORD dwAddress, BYTE *Data);
bool WriteCartMBC1(LPGBCART Cart, WORD dwAddress, BYTE *Data);
bool ReadCartMBC2(LPGBCART Cart, WORD dwAddress, BYTE *Data);
bool WriteCartMBC2(LPGBCART Cart, WORD dwAddress, BYTE *Data);
bool ReadCartMBC3(LPGBCART Cart, WORD dwAddress, BYTE *Data);
bool WriteCartMBC3(LPGBCART Cart, WORD dwAddress, BYTE *Data);
bool ReadCartMBC5(LPGBCART Cart, WORD dwAddress, BYTE *Data);
bool WriteCartMBC5(LPGBCART Cart, WORD dwAddress, BYTE *Data);

// Tries to read RTC data from separate file (not integrated into SAV)
//		success sets the useTDF flag
//		failure inits the RTC at zero and maybe throws a warning
void ReadTDF(LPGBCART Cart) {
}

void WriteTDF(LPGBCART Cart) {
	// check useTDF flag
	// write data from RTC to TDF file
}

void UpdateRTC(LPGBCART Cart) {
	time_t now, dif;
	int days;

	now = time(NULL);
	dif = now - Cart->timerLastUpdate;

	Cart->TimerData[0] += (BYTE)(dif % 60);
	dif /= 60;
	Cart->TimerData[1] += (BYTE)(dif % 60);
	dif /= 60;
	Cart->TimerData[2] += (BYTE)(dif % 24);
	dif /= 24;

	days = Cart->TimerData[3] + ((Cart->TimerData[4] & 1) << 8) + (int)dif;
	Cart->TimerData[3] = (days & 0xFF);

	if(days > 255) {
		if(days > 511) {
			days &= 511;
			Cart->TimerData[4] |= 0x80;
		}
		if (days > 255)
		Cart->TimerData[4] = (Cart->TimerData[4] & 0xFE) | (days > 255 ? 1 : 0);
	}

	Cart->timerLastUpdate = now;

	DebugWriteA("Update RTC: ");
	DebugWriteByteA(Cart->TimerData[0]);
	DebugWriteA(":");
	DebugWriteByteA(Cart->TimerData[1]);
	DebugWriteA(":");
	DebugWriteByteA(Cart->TimerData[2]);
	DebugWriteA(":");
	DebugWriteByteA(Cart->TimerData[3]);
	DebugWriteA(":");
	DebugWriteByteA(Cart->TimerData[4]);
	DebugWriteA("\n");
}

/*
Cart --> the LPGBCART structure to operate on. RamData must already be loaded - if Goomba is detected it will be reloaded from hTemp.
hTemp --> An open file handle. File pointer will be reset to the beginning of the file when the function starts.
NumQuarterBlocks --> Used to calculate the expected RAM size for this ROM.
RamFileName --> If loading Goomba is successful, this string will be copied to Cart->hGoombaRamPath, unless it is NULL. Use NULL here if you want the save file to be read-only.
*/
void GoombaCheckAndLoad(LPGBCART Cart, HANDLE hTemp, DWORD NumQuarterBlocks, LPCTSTR RamFileName) {
	if (((uint32_t*)Cart->RamData)[0] == GOOMBA_STATEID) {
		UnmapViewOfFile(Cart->RamData);
		CloseHandle(Cart->hRamFile);
		Cart->hRamFile = NULL;
		Cart->RamData = NULL;

		char tmpbuffer[GOOMBA_COLOR_SRAM_SIZE];
		DWORD b_read;
		SetFilePointer(hTemp, 0, NULL, FILE_BEGIN);
		ReadFile(hTemp, tmpbuffer, GOOMBA_COLOR_SRAM_SIZE, &b_read, NULL);

		memcpy(Cart->GoombaHeaderTitle, &Cart->RomData[0x134], 0x0F);
		Cart->GoombaHeaderTitle[0x0F] = '\0';

		size_t size_needed = NumQuarterBlocks * 0x0800 + ((Cart->bHasTimer && Cart->bHasBattery) ? sizeof(gbCartRTC) : 0);
		Cart->RamData = (LPBYTE)P_malloc(size_needed);

		Cart->sGoombaRamPath = NULL;
		stateheader* sh = stateheader_for(tmpbuffer, Cart->GoombaHeaderTitle);
		if (sh == NULL) {
			ClearData(Cart->RamData, size_needed);
			WarningMessage(IDS_ERR_GBSRAMERR, MB_OK | MB_ICONWARNING);
		} else {
			goomba_size_t extracted_size;
			void* gbc_data = goomba_extract(tmpbuffer, sh, &extracted_size);
			if (gbc_data != NULL) {
				Cart->sGoombaRamPath = RamFileName == NULL ? NULL : _tcsdup(RamFileName);
				Cart->iGoombaRamSize = extracted_size;
				if (extracted_size > size_needed) {
					ClearData(Cart->RamData, size_needed);
					WarningMessage(IDS_ERR_GBSRAMERR, MB_OK | MB_ICONWARNING);
				} else {
					memcpy(Cart->RamData, gbc_data, extracted_size);
					// if we were going to fake the rtc data we would probably do it here
				}
			}
		}
	}
}

bool UpdateGoombaFile(LPGBCART Cart) {
	HANDLE h = NULL;
	LPVOID gba_data = NULL;
	void* new_gba_data = NULL;

	h = CreateFile(Cart->sGoombaRamPath, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
	if (h == INVALID_HANDLE_VALUE) {
		DebugWriteA("Invalid handle for %s\n", Cart->sGoombaRamPath);
		goto dispose;
	}
	// Not sure the lock is needed - if transfer paks are closed sequentially we'll be okay
	/*OVERLAPPED o1;
	o1.Offset = 0;
	o1.OffsetHigh = 0;
	o1.hEvent = 0;
	if (!LockFileEx(h, LOCKFILE_EXCLUSIVE_LOCK, 0, GOOMBA_COLOR_SRAM_SIZE, 0, &o1)) {
		DebugWriteA("Lock error\n");
		goto dispose;
	}*/

	gba_data = P_malloc(GOOMBA_COLOR_SRAM_SIZE);
	DWORD b_read;
	ReadFile(h, gba_data, GOOMBA_COLOR_SRAM_SIZE, &b_read, NULL);

	stateheader* sh = stateheader_for(gba_data, Cart->GoombaHeaderTitle);
	if (sh == NULL) {
		DebugWriteA("[goombasav] %s\n", goomba_last_error());
		goto dispose;
	}

	new_gba_data = goomba_new_sav(gba_data, sh, Cart->RamData, Cart->iGoombaRamSize);
	P_free(gba_data);
	gba_data = NULL;
	if (new_gba_data == NULL) {
		DebugWriteA("[goombasav] %s\n", goomba_last_error());
		goto dispose;
	}
	if (SetFilePointer(h, 0, NULL, FILE_BEGIN) == INVALID_SET_FILE_POINTER && GetLastError() != NO_ERROR) {
		DebugWriteA("SetFilePointer error %d\n", GetLastError());
		goto dispose;
	}
	if (!WriteFile(h, new_gba_data, GOOMBA_COLOR_SRAM_SIZE, &b_read, NULL)) {
		DebugWriteA("File write error %d\n", GetLastError());
		goto dispose;
	}
	DebugWriteA("Wrote Goomba save file\n");
	free(new_gba_data);
	//UnlockFileEx(h, 0, GOOMBA_COLOR_SRAM_SIZE, 0, &o1);
	CloseHandle(h);
	return true;
dispose:
	// This error is very rare - usually if the file cannot be saved to, the program will detect that it's read-only
	MessageBoxA(NULL, "Unable to update the Goomba save file. Your progress will not be saved.", "Error", MB_OK | MB_ICONERROR);
	if (new_gba_data != NULL) free(new_gba_data);
	if (gba_data != NULL) P_free(gba_data);
	if (h != NULL) {
		//UnlockFileEx(h, 0, GOOMBA_COLOR_SRAM_SIZE, 0, &o1);
		CloseHandle(h);
	}
	return false;
}

// returns true if the ROM was loaded OK
bool LoadCart(LPGBCART Cart, LPCTSTR RomFileName, LPCTSTR RamFileName, LPCTSTR TdfFileName)
{
	HANDLE hTemp;
	DWORD dwFilesize;
	DWORD NumQuarterBlocks = 0;

	UnloadCart(Cart);	// first, make sure any previous carts have been unloaded

	Cart->iCurrentRamBankNo = 0;
	Cart->iCurrentRomBankNo = 1;
	Cart->bRamEnableState = 0;
	Cart->bMBC1RAMbanking = 0;

	// Attempt to load the ROM file.
	hTemp = CreateFile(RomFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_EXISTING, 0, NULL);
	if (hTemp != INVALID_HANDLE_VALUE && (Cart->hRomFile = CreateFileMapping(hTemp, NULL, PAGE_READONLY, 0, 0, NULL) ) )
	{
		// if the first case fails, the file doesn't exist. The second case can fail if the file size is zero.
		dwFilesize = GetFileSize(hTemp, NULL);
		CloseHandle(hTemp);
		Cart->RomData = (LPCBYTE)MapViewOfFile( Cart->hRomFile, FILE_MAP_READ, 0, 0, 0 );
	} else {
		DebugWriteA("Couldn't load the ROM file, GetLastError returned %08x\n", GetLastError());
		if (hTemp != INVALID_HANDLE_VALUE)
			CloseHandle(hTemp);	// if file size was zero, make sure we don't leak the handle

		ErrorMessage(IDS_ERR_GBROM, 0, false);
		return false;
	}

	if (dwFilesize < 0x8000) // a Rom file has to be at least 32kb
	{
		DebugWriteA("ROM file wasn't big enough to be a GB ROM!\n");
		ErrorMessage(IDS_ERR_GBROM, 0, false);

		UnloadCart(Cart);
		return false;
	}

	DebugWriteA(" Cartridge Type #:");
	DebugWriteByteA(Cart->RomData[0x147]);
	DebugWriteA("\n");
	switch (Cart->RomData[0x147]) {	// if we hadn't checked the file size before, this might have caused an access violation
	case 0x00:
		Cart->iCartType = GB_NORM;
		Cart->bHasRam = false;
		Cart->bHasBattery = false;
		Cart->bHasTimer = false;
		Cart->bHasRumble = false;
		break;
	case 0x01:
		Cart->iCartType = GB_MBC1;
		Cart->bHasRam = false;
		Cart->bHasBattery = false;
		Cart->bHasTimer = false;
		Cart->bHasRumble = false;
		break;
	case 0x02:
		Cart->iCartType = GB_MBC1;
		Cart->bHasRam = true;
		Cart->bHasBattery = false;
		Cart->bHasTimer = false;
		Cart->bHasRumble = false;
		break;
	case 0x03:
		Cart->iCartType = GB_MBC1;
		Cart->bHasRam = true;
		Cart->bHasBattery = true;
		Cart->bHasTimer = false;
		Cart->bHasRumble = false;
		break;
	case 0x05:
		Cart->iCartType = GB_MBC2;
		Cart->bHasRam = false;
		Cart->bHasBattery = false;
		Cart->bHasTimer = false;
		Cart->bHasRumble = false;
		break;
	case 0x06:
		Cart->iCartType = GB_MBC2;
		Cart->bHasRam = false;
		Cart->bHasBattery = true;
		Cart->bHasTimer = false;
		Cart->bHasRumble = false;
		break;
	case 0x08:
		Cart->iCartType = GB_NORM;
		Cart->bHasRam = true;
		Cart->bHasBattery = false;
		Cart->bHasTimer = false;
		Cart->bHasRumble = false;
		break;
	case 0x09:
		Cart->iCartType = GB_NORM;
		Cart->bHasRam = true;
		Cart->bHasBattery = true;
		Cart->bHasTimer = false;
		Cart->bHasRumble = false;
		break;
	case 0x0B:
		Cart->iCartType = GB_MMMO1;
		Cart->bHasRam = false;
		Cart->bHasBattery = false;
		Cart->bHasTimer = false;
		Cart->bHasRumble = false;
		break;
	case 0x0C:
		Cart->iCartType = GB_MMMO1;
		Cart->bHasRam = true;
		Cart->bHasBattery = false;
		Cart->bHasTimer = false;
		Cart->bHasRumble = false;
		break;
	case 0x0D:
		Cart->iCartType = GB_MMMO1;
		Cart->bHasRam = true;
		Cart->bHasBattery = true;
		Cart->bHasTimer = false;
		Cart->bHasRumble = false;
		break;
	case 0x0F:
		Cart->iCartType = GB_MBC3;
		Cart->bHasRam = false;
		Cart->bHasBattery = true;
		Cart->bHasTimer = true;
		Cart->bHasRumble = false;
		break;
	case 0x10:
		Cart->iCartType = GB_MBC3;
		Cart->bHasRam = true;
		Cart->bHasBattery = true;
		Cart->bHasTimer = true;
		Cart->bHasRumble = false;
		break;
	case 0x11:
		Cart->iCartType = GB_MBC3;
		Cart->bHasRam = false;
		Cart->bHasBattery = false;
		Cart->bHasTimer = false;
		Cart->bHasRumble = false;
		break;
	case 0x12:
		Cart->iCartType = GB_MBC3;
		Cart->bHasRam = true;
		Cart->bHasBattery = true;
		Cart->bHasTimer = false;
		Cart->bHasRumble = false;
		break;
	case 0x13:
		Cart->iCartType = GB_MBC3;
		Cart->bHasRam = true;
		Cart->bHasBattery = true;
		Cart->bHasTimer = false;
		Cart->bHasRumble = false;
		break;
	case 0x19:
		Cart->iCartType = GB_MBC5;
		Cart->bHasRam = false;
		Cart->bHasBattery = false;
		Cart->bHasTimer = false;
		Cart->bHasRumble = false;
		break;
	case 0x1A:
		Cart->iCartType = GB_MBC5;
		Cart->bHasRam = true;
		Cart->bHasBattery = false;
		Cart->bHasTimer = false;
		Cart->bHasRumble = false;
		break;
	case 0x1B:
		Cart->iCartType = GB_MBC5;
		Cart->bHasRam = true;
		Cart->bHasBattery = true;
		Cart->bHasTimer = false;
		Cart->bHasRumble = false;
		break;
	case 0x1C:
		Cart->iCartType = GB_MBC5;
		Cart->bHasRam = false;
		Cart->bHasBattery = false;
		Cart->bHasTimer = false;
		Cart->bHasRumble = true;
		break;
	case 0x1D:
		Cart->iCartType = GB_MBC5;
		Cart->bHasRam = true;
		Cart->bHasBattery = false;
		Cart->bHasTimer = false;
		Cart->bHasRumble = true;
		break;
	case 0x1E:
		Cart->iCartType = GB_MBC5;
		Cart->bHasRam = true;
		Cart->bHasBattery = true;
		Cart->bHasTimer = false;
		Cart->bHasRumble = true;
		break;
	default:
		WarningMessage( IDS_ERR_GBROM, MB_OK | MB_ICONWARNING);
		DebugWriteA("TPak: unsupported paktype\n");
		UnloadCart(Cart);
		return false;
	}

	// assign read/write handlers
	switch (Cart->iCartType) {
	case GB_NORM: // Raw cartridge
		Cart->ptrfnReadCart = &ReadCartNorm;
		Cart->ptrfnWriteCart = &WriteCartNorm;
		break;
	case GB_MBC1:
		Cart->ptrfnReadCart =  &ReadCartMBC1;
		Cart->ptrfnWriteCart = &WriteCartMBC1;
		break;
	case GB_MBC2:
		Cart->ptrfnReadCart =  &ReadCartMBC2;
		Cart->ptrfnWriteCart = &WriteCartMBC2;
		break;
	case GB_MBC3:
		Cart->ptrfnReadCart =  &ReadCartMBC3;
		Cart->ptrfnWriteCart = &WriteCartMBC3;
		break;
	case GB_MBC5:
		Cart->ptrfnReadCart =  &ReadCartMBC5;
		Cart->ptrfnWriteCart = &WriteCartMBC5;
		break;
	default: // Don't pretend we know how to handle carts we don't support
		Cart->ptrfnReadCart = NULL;
		Cart->ptrfnWriteCart = NULL;
		DebugWriteA("Unsupported paktype: can't read/write cart type %02X\n", Cart->iCartType);
		UnloadCart(Cart);
		return false;
	}

	// Determine ROM size for paging checks
	Cart->iNumRomBanks = 2;
	switch (Cart->RomData[0x148]) {
	case 0x01:
		Cart->iNumRomBanks = 4;
		break;
	case 0x02:
		Cart->iNumRomBanks = 8;
		break;
	case 0x03:
		Cart->iNumRomBanks = 16;
		break;
	case 0x04:
		Cart->iNumRomBanks = 32;
		break;
	case 0x05:
		Cart->iNumRomBanks = 64;
		break;
	case 0x06:
		Cart->iNumRomBanks = 128;
		break;
	case 0x52:
		Cart->iNumRomBanks = 72;
		break;
	case 0x53:
		Cart->iNumRomBanks = 80;
		break;
	case 0x54:
		Cart->iNumRomBanks = 96;
		break;
	}

	if (dwFilesize != 0x4000 * Cart->iNumRomBanks) // Now that we know how big the ROM is supposed to be, check it again
	{
		ErrorMessage(IDS_ERR_GBROM, 0, false);

		UnloadCart(Cart);
		return false;
	}

	// Determine RAM size for paging checks
	Cart->iNumRamBanks = 0;
	switch (Cart->RomData[0x149]) {
	case 0x01:
		Cart->iNumRamBanks = 1;
		NumQuarterBlocks = 1;
		break;
	case 0x02:
		Cart->iNumRamBanks = 1;
		NumQuarterBlocks = 4;
		break;
	case 0x03:
		Cart->iNumRamBanks = 4;
		NumQuarterBlocks = 16;
		break;
	case 0x04:
		Cart->iNumRamBanks = 16;
		NumQuarterBlocks = 64;
		break;
	case 0x05:
		Cart->iNumRamBanks = 8;
		NumQuarterBlocks = 32;
		break;
	}

	DebugWriteA("GB cart has %d ROM banks, %d RAM quarter banks\n", Cart->iNumRomBanks, NumQuarterBlocks);
	if (Cart->bHasTimer)
	{
		DebugWriteA("GB cart timer present\n");
	}

	// Attempt to load the SRAM file, but only if RAM is supposed to be present.
	// For saving back to a file, if we map too much it will expand the file.
	if (Cart->bHasRam)
	{
		if (Cart->bHasBattery)
		{
			hTemp = CreateFile( RamFileName, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL );
			if( hTemp == INVALID_HANDLE_VALUE )
			{// test if Read-only access is possible
				hTemp = CreateFile( RamFileName, GENERIC_READ, FILE_SHARE_READ, NULL, OPEN_ALWAYS, 0, NULL );
				if (Cart->bHasTimer && Cart->bHasBattery) {
					Cart->RamData = (LPBYTE)P_malloc(NumQuarterBlocks * 0x0800 + sizeof(gbCartRTC));
					ClearData(Cart->RamData, NumQuarterBlocks * 0x0800 + sizeof(gbCartRTC));
				}
				else {
					Cart->RamData = (LPBYTE)P_malloc(NumQuarterBlocks * 0x0800);
					ClearData(Cart->RamData, NumQuarterBlocks * 0x0800);
				}

				if( hTemp != INVALID_HANDLE_VALUE )
				{
					DWORD dwBytesRead;

					if (Cart->bHasTimer && Cart->bHasBattery)
						ReadFile(hTemp, Cart->RamData, NumQuarterBlocks * 0x0800 + sizeof(gbCartRTC), &dwBytesRead, NULL);
					else
						ReadFile(hTemp, Cart->RamData, NumQuarterBlocks * 0x0800, &dwBytesRead, NULL);
					GoombaCheckAndLoad(Cart, hTemp, NumQuarterBlocks, NULL);
					WarningMessage( IDS_DLG_TPAK_READONLY, MB_OK | MB_ICONWARNING);
				}
				else
				{
					WarningMessage( IDS_ERR_GBSRAMERR, MB_OK | MB_ICONWARNING);
					return true;
				}
			} else { // file is OK, use a mapping
				if (Cart->bHasTimer && Cart->bHasBattery)
					Cart->hRamFile = CreateFileMapping( hTemp, NULL, PAGE_READWRITE, 0, NumQuarterBlocks * 0x0800 + sizeof(gbCartRTC), NULL);
				else
					Cart->hRamFile = CreateFileMapping( hTemp, NULL, PAGE_READWRITE, 0, NumQuarterBlocks * 0x0800, NULL);

				if (Cart->hRamFile != NULL)
				{
					Cart->RamData = (LPBYTE)MapViewOfFile( Cart->hRamFile, FILE_MAP_ALL_ACCESS, 0, 0, 0 );
					GoombaCheckAndLoad(Cart, hTemp, NumQuarterBlocks, RamFileName);
				} else { // could happen, if the file isn't big enough AND can't be grown to fit
					DWORD dwBytesRead;
					if (Cart->bHasTimer && Cart->bHasBattery) {
						Cart->RamData = (LPBYTE)P_malloc(NumQuarterBlocks * 0x0800 + sizeof(gbCartRTC));
						ReadFile(hTemp, Cart->RamData, NumQuarterBlocks * 0x0800 + sizeof(gbCartRTC), &dwBytesRead, NULL);
					} else {
						Cart->RamData = (LPBYTE)P_malloc(NumQuarterBlocks * 0x0800);
						ReadFile(hTemp, Cart->RamData, NumQuarterBlocks * 0x0800, &dwBytesRead, NULL);
					}
					if (dwBytesRead < NumQuarterBlocks * 0x0800 + ((Cart->bHasTimer && Cart->bHasBattery) ? sizeof(gbCartRTC) : 0))
					{
						ClearData(Cart->RamData, NumQuarterBlocks * 0x0800 + ((Cart->bHasTimer && Cart->bHasBattery) ? sizeof(gbCartRTC) : 0));
						WarningMessage( IDS_ERR_GBSRAMERR, MB_OK | MB_ICONWARNING);
					}
					else
					{
						GoombaCheckAndLoad(Cart, hTemp, NumQuarterBlocks, NULL);
						WarningMessage( IDS_DLG_TPAK_READONLY, MB_OK | MB_ICONWARNING);
					}
				}
			}

			if (Cart->bHasTimer && Cart->bHasBattery) {
				dwFilesize = Cart->sGoombaRamPath != NULL
					? Cart->iGoombaRamSize
					: GetFileSize(hTemp, 0);
				if (dwFilesize >= (NumQuarterBlocks * 0x0800 + sizeof(gbCartRTC) ) ) {
					// Looks like there is extra data in the SAV file than just RAM data... assume it is RTC data.
					gbCartRTC RTCTimer;
					CopyMemory( &RTCTimer, &Cart->RamData[NumQuarterBlocks * 0x0800], sizeof(RTCTimer) );
					Cart->TimerData[0] = (BYTE)RTCTimer.mapperSeconds;
					Cart->TimerData[1] = (BYTE)RTCTimer.mapperMinutes;
					Cart->TimerData[2] = (BYTE)RTCTimer.mapperHours;
					Cart->TimerData[3] = (BYTE)RTCTimer.mapperDays;
					Cart->TimerData[4] = (BYTE)RTCTimer.mapperControl;
					Cart->LatchedTimerData[0] = (BYTE)RTCTimer.mapperLSeconds;
					Cart->LatchedTimerData[1] = (BYTE)RTCTimer.mapperLMinutes;
					Cart->LatchedTimerData[2] = (BYTE)RTCTimer.mapperLHours;
					Cart->LatchedTimerData[3] = (BYTE)RTCTimer.mapperLDays;
					Cart->LatchedTimerData[4] = (BYTE)RTCTimer.mapperLControl;
					Cart->timerLastUpdate = RTCTimer.mapperLastTime;
					UpdateRTC(Cart);
				}
				else {
					ReadTDF(Cart);	// try to open TDF format, clear/init Cart->TimerData if that fails
				}
			}

			CloseHandle(hTemp);
		} else {
			// no battery; just allocate some RAM
			Cart->RamData = (LPBYTE)P_malloc(Cart->iNumRamBanks * 0x2000);
		}
	}

	Cart->TimerDataLatched = false;

	return true;
}

// Done
bool ReadCartNorm(LPGBCART Cart, WORD dwAddress, BYTE *Data) // For all non-MBC carts; fixed 0x8000 ROM; fixed, optional 0x2000 RAM
{
	switch (dwAddress >> 13) // hack: examine highest 3 bits
	{
	case 0:
	case 1:
	case 2:
	case 3:	//	if ((dwAddress >= 0) && (dwAddress <= 0x7FFF))
		CopyMemory(Data, &Cart->RomData[dwAddress], 32);
		DebugWriteA("Nonbanked ROM read - RAW\n");
		break;
	case 5:
		if (Cart->bHasRam)	// no MBC, so no enable state to check
		{
			if (Cart->RomData[0x149] == 1 && (dwAddress - 0xA000) / 0x0800 ) // Only 1/4 of the RAM space is used, and we're out of bounds
			{
				DebugWriteA("Failed RAM read: Unbanked (out of bounds)");
				ZeroMemory(Data, 32);
			}
			else
			{
				CopyMemory(Data, &Cart->RamData[dwAddress - 0xA000], 32);
				DebugWriteA("RAM read: Unbanked\n");
			}
		}
		else
		{
			ZeroMemory(Data, 32);
			DebugWriteA("Failed RAM read: Unbanked (RAM not present)\n");
		}
		break;
	default:
		DebugWriteA("Bad read from RAW cart, address %04X\n", dwAddress);
	}

	return true;
}

// Done
bool WriteCartNorm(LPGBCART Cart, WORD dwAddress, BYTE *Data)
{
	if (!Cart->bHasRam)
	{
		DebugWriteA("RAM write: no RAM\n");
		return true;
	}

	if (Cart->RomData[0x149] == 1) { // Whoops... Only 1/4 of the RAM space is used.
		if ((dwAddress >= 0xA000) && (dwAddress <= 0xA7FF)) { // Write to RAM
			DebugWriteA("RAM write: Unbanked\n");
			CopyMemory(&Cart->RamData[dwAddress - 0xA000], Data, 32);
		}
		else
		{
			DebugWriteA("RAM write: Unbanked (out of range!)\n");
		}
	} else {
		if ((dwAddress >= 0xA000) && (dwAddress <= 0xBFFF)) { // Write to RAM
			DebugWriteA("RAM write: Unbanked\n");
			CopyMemory(&Cart->RamData[dwAddress - 0xA000], Data, 32);
		}
	}
	return true;
}

// Done
bool ReadCartMBC1(LPGBCART Cart, WORD dwAddress, BYTE *Data)
{
	switch (dwAddress >> 13)
	{
	case 0:
	case 1:	//	if ((dwAddress >= 0) && (dwAddress <= 0x3FFF))
		CopyMemory(Data, &Cart->RomData[dwAddress], 32);
		DebugWriteA("Nonbanked ROM read - MBC1\n");
		break;
	case 2:
	case 3:	//	else if ((dwAddress >= 0x4000) && (dwAddress <= 0x7FFF))
		if (Cart->iCurrentRomBankNo >= Cart->iNumRomBanks)
		{
			ZeroMemory(Data, 32);
			DebugWriteA("Banked ROM read: (Banking Error) Bank %02X\n", Cart->iCurrentRomBankNo);
		}
		else
		{
			// for (i=0; i<32; i++) Data[i] = Cart->RomData[(dwAddress - 0x4000) + i + (Cart->iCurrentRomBankNo * 0x4000)];
			CopyMemory(Data, &Cart->RomData[dwAddress - 0x4000 + (Cart->iCurrentRomBankNo << 14)], 32);
			DebugWriteA("Banked ROM read: Bank %02X\n", Cart->iCurrentRomBankNo);
		}
		break;
	case 5:	//	else if ((dwAddress >= 0xA000) && (dwAddress <= 0xBFFF))
		if (Cart->bHasRam){ // && Cart->bRamEnableState) {
			if (Cart->iCurrentRamBankNo >= Cart->iNumRamBanks) {
				ZeroMemory(Data, 32);
				DebugWriteA("Failed RAM read: (Banking Error) %02X\n", Cart->iCurrentRamBankNo);
			} else {
				CopyMemory(Data, &Cart->RamData[dwAddress - 0xA000 + (Cart->iCurrentRamBankNo << 13)], 32);
				DebugWriteA("RAM read: Bank %02X\n", Cart->iCurrentRamBankNo);
			}
		} else {
			ZeroMemory(Data, 32);
			DebugWriteA("Failed RAM read: (RAM not present)\n");
		}
		break;
	default:
		DebugWriteA("Bad read from MBC1 cart, address %04X\n", dwAddress);
	}

	return true;
}

// Done
bool WriteCartMBC1(LPGBCART Cart, WORD dwAddress, BYTE *Data)
{
	switch (dwAddress >> 13)
	{
	case 0:	//	if ((dwAddress >= 0) && (dwAddress <= 0x1FFF)) // RAM enable
		Cart->bRamEnableState = (Data[0] == 0x0A);
		DebugWriteA("Set RAM enable: %d\n", Cart->bRamEnableState);
		break;
	case 1:	//	else if ((dwAddress >= 0x2000) && (dwAddress <= 0x3FFF)) // ROM bank select
		Cart->iCurrentRomBankNo &= 0x60;	// keep MSB
		Cart->iCurrentRomBankNo |= Data[0] & 0x1F;

		// emulate quirk: 0x00 -> 0x01, 0x20 -> 0x21, 0x40->0x41, 0x60 -> 0x61
		if ((Cart->iCurrentRomBankNo & 0x1F) == 0) {
			Cart->iCurrentRomBankNo |= 0x01;
		}
		DebugWriteA("Set ROM Bank: %02X\n", Cart->iCurrentRomBankNo);
		break;
	case 2:	//	else if ((dwAddress >= 0x4000) && (dwAddress <= 0x5FFF)) // RAM bank select
		if (Cart->bMBC1RAMbanking)	{
			Cart->iCurrentRamBankNo = Data[0] & 0x03;
			DebugWriteA("Set RAM Bank: %02X\n", Cart->iCurrentRamBankNo);
		}
		else {
			Cart->iCurrentRomBankNo &= 0x1F;
			Cart->iCurrentRomBankNo |= ((Data[0] & 0x03) << 5); // set bits 5 and 6 of ROM bank
			DebugWriteA("Set ROM Bank MSB, ROM bank now: %02X\n", Cart->iCurrentRomBankNo);
		}
		break;
	case 3:	//	else if ((dwAddress >= 0x6000) && (dwAddress <= 0x7FFF)) // MBC1 mode select
		// this is overly complicated, but it keeps us from having to do bitwise math later
		// Basically we shuffle the 2 "magic bits" between iCurrentRomBankNo and iCurrentRamBankNo as necessary.
		if (Cart->bMBC1RAMbanking != (Data[0] & 0x01)) {
			// we should only alter the ROM and RAM bank numbers if we have changed modes
			Cart->bMBC1RAMbanking = Data[0] & 0x01;
			if (Cart->bMBC1RAMbanking)
			{
				Cart->iCurrentRamBankNo = Cart->iCurrentRomBankNo >> 5;	// set the ram bank to the "magic bits"
				Cart->iCurrentRomBankNo &= 0x1F; // zero out bits 5 and 6 to keep consistency
			}
			else
			{
				Cart->iCurrentRomBankNo &= 0x1F;
				Cart->iCurrentRomBankNo |= (Cart->iCurrentRamBankNo << 5);
				Cart->iCurrentRamBankNo = 0x00;	// we can only reach RAM page 0
			}
			DebugWriteA("Set MBC1 mode: %s\n", Cart->bMBC1RAMbanking ? "ROMbanking" : "RAMbanking" );
		}
		else
		{
			DebugWriteA("Already in MBC1 mode: %s\n", Cart->bMBC1RAMbanking ? "ROMbanking" : "RAMbanking" );
		}

		break;
	case 5:	// else if ((dwAddress >= 0xA000) && (dwAddress <= 0xBFFF)) // Write to RAM
		if (Cart->bHasRam) // && Cart->bRamEnableState)
		{
			DebugWriteA("RAM write: Bank %02X\n", Cart->iCurrentRamBankNo);
			CopyMemory(&Cart->RamData[dwAddress - 0xA000 + (Cart->iCurrentRamBankNo << 13)], Data, 32);
		}
		else
		{
			DebugWriteA("Failed RAM write: (RAM not present)\n");
		}
		break;
	default:
		DebugWriteA("Bad write to MBC1 cart, address %04X\n", dwAddress);
	}

	return true;
}

// Done
bool ReadCartMBC2(LPGBCART Cart, WORD dwAddress, BYTE *Data)
{
	switch (dwAddress >> 13)
	{
	case 0:
	case 1: // if ((dwAddress <= 0x3FFF))
		CopyMemory(Data, &Cart->RomData[dwAddress], 32);
		DebugWriteA("Nonbanked ROM read - MBC2\n");
		break;
	case 2:
	case 3:	//	else if ((dwAddress >= 0x4000) && (dwAddress <= 0x7FFF))
		if (Cart->iCurrentRomBankNo >= Cart->iNumRomBanks) {
			ZeroMemory(Data, 32);
			DebugWriteA("Banked ROM read: (Banking Error) %02X\n", Cart->iCurrentRomBankNo);
		} else {
			CopyMemory(Data, &Cart->RomData[dwAddress - 0x4000 + (Cart->iCurrentRomBankNo << 14)], 32);
			DebugWriteA("Banked ROM read: Bank %02X\n", Cart->iCurrentRomBankNo);
		}
		break;
	case 5:	//	else if ((dwAddress >= 0xA000) && (dwAddress <= 0xBFFF))
		if (Cart->bHasRam && Cart->bRamEnableState) {
			CopyMemory(Data, &Cart->RamData[dwAddress - 0xA000], 32);
			DebugWriteA("RAM read: Unbanked\n");
		} else {
			ZeroMemory(Data, 32);
			DebugWriteA("Failed RAM read: (RAM not present or not active)\n");
		}
		break;
	default:
		DebugWriteA("Bad read from MBC2 cart, address %04X\n", dwAddress);
	}

	return true;
}

// Done
bool WriteCartMBC2(LPGBCART Cart, WORD dwAddress, BYTE *Data)
{
	switch (dwAddress >> 13)
	{
	case 0: //	if ((dwAddress <= 0x1FFF)) // We shouldn't be able to read/write to RAM unless this is toggled on
		Cart->bRamEnableState = (Data[0] == 0x0A);
		DebugWriteA("Set RAM enable: %d\n", Cart->bRamEnableState);
		break;
	case 1: //	else if ((dwAddress >= 0x2000) && (dwAddress <= 0x3FFF)) // ROM bank select
		Cart->iCurrentRomBankNo = Data[0] & 0x0F;
		if (Cart->iCurrentRomBankNo == 0) {
			Cart->iCurrentRomBankNo = 1;
		}
		DebugWriteA("Set ROM Bank: %02X\n", Cart->iCurrentRomBankNo);
		break;
	case 2: //	if ((dwAddress >= 0x4000) && (dwAddress <= 0x5FFF)) // RAM bank select
		if (Cart->bHasRam) {
			Cart->iCurrentRamBankNo = Data[0] & 0x07;
			DebugWriteA("Set RAM Bank: %02X\n", Cart->iCurrentRamBankNo);
		}
		break;
	case 5: //	else if ((dwAddress >= 0xA000) && (dwAddress <= 0xBFFF) && Cart->bRamEnableState) // Write to RAM
		if (Cart->bHasRam) {
			DebugWriteA("RAM write: Bank %02X\n", Cart->iCurrentRamBankNo);
			CopyMemory(&Cart->RamData[dwAddress - 0xA000 + (Cart->iCurrentRamBankNo << 13)], Data, 32);
		}
		break;
	default:
		DebugWriteA("Bad write to MBC2 cart, address %04X\n", dwAddress);
	}

	return true;
}

// Done
bool ReadCartMBC3(LPGBCART Cart, WORD dwAddress, BYTE *Data)
{
	int i;

	switch (dwAddress >> 13)
	{
	case 0:
	case 1: //	if ((dwAddress <= 0x3FFF))
		CopyMemory(Data, &Cart->RomData[dwAddress], 32);
		DebugWriteA("Nonbanked ROM read - MBC3\n");
		break;
	case 2:
	case 3: //	else if ((dwAddress >= 0x4000) && (dwAddress <= 0x7FFF))
		if (Cart->iCurrentRomBankNo >= Cart->iNumRomBanks) {
			ZeroMemory(Data, 32);
			DebugWriteA("Banked ROM read: (Banking Error) %02X\n", Cart->iCurrentRomBankNo);
		} else {
			CopyMemory(Data, &Cart->RomData[dwAddress - 0x4000 + (Cart->iCurrentRomBankNo * 0x4000)], 32);
			DebugWriteA("Banked ROM read: Bank %02X\n", Cart->iCurrentRomBankNo);
		}
		break;
	case 5: //	else if ((dwAddress >= 0xA000) && (dwAddress <= 0xBFFF))
		if (Cart->bHasTimer && (Cart->iCurrentRamBankNo >= 0x08 && Cart->iCurrentRamBankNo <= 0x0c)) {
			// w00t! the Timer was just read!!
			if (Cart->TimerDataLatched) {
				for (i=0; i<32; i++)
					Data[i] = Cart->LatchedTimerData[Cart->iCurrentRamBankNo - 0x08];
			} else {
				UpdateRTC(Cart);
				for (i=0; i<32; i++)
					Data[i] = Cart->TimerData[Cart->iCurrentRamBankNo - 0x08];
			}
		}
		else if (Cart->bHasRam) {
			if (Cart->iCurrentRamBankNo >= Cart->iNumRamBanks) {
				ZeroMemory(Data, 32);
				DebugWriteA("Failed RAM read: (Banking Error) %02X\n", Cart->iCurrentRamBankNo);
			}
			else {
				CopyMemory(Data, &Cart->RamData[dwAddress - 0xA000 + (Cart->iCurrentRamBankNo * 0x2000)], 32);
				DebugWriteA("RAM read: Bank %02X\n", Cart->iCurrentRamBankNo);
			}
			//else {
			//	ZeroMemory(Data, 32);
			//	//for (i=0; i<32; i++) Data[i] = 0;
			//	DebugWriteA("Failed RAM read: (RAM not active)\n");
			//}
		} else {
			ZeroMemory(Data, 32);
			DebugWriteA("Failed RAM read: (RAM not present)\n");
		}
		break;
	default:
		DebugWriteA("Bad read from MBC3 cart, address %04X\n", dwAddress);
	}

	return true;
}

// Done
bool WriteCartMBC3(LPGBCART Cart, WORD dwAddress, BYTE *Data)
{
	int i;

	switch (dwAddress >> 13)
	{
	case 0: //	if ((dwAddress <= 0x1FFF)) // We shouldn't be able to read/write to RAM unless this is toggled on
		Cart->bRamEnableState = (Data[0] == 0x0A);
		DebugWriteA("Set RAM enable: %d\n", Cart->bRamEnableState);
		break;
	case 1: //	else if ((dwAddress >= 0x2000) && (dwAddress <= 0x3FFF)) // ROM bank select
		Cart->iCurrentRomBankNo = Data[0] & 0x7F;
		if (Cart->iCurrentRomBankNo == 0) {
			Cart->iCurrentRomBankNo = 1;
		}
		DebugWriteA("Set Rom Bank: %02X\n", Cart->iCurrentRomBankNo);
		break;
	case 2: //	if ((dwAddress >= 0x4000) && (dwAddress <= 0x5FFF)) // RAM/Clock bank select
		if (Cart->bHasRam) {
			Cart->iCurrentRamBankNo = Data[0] & 0x03;
			DebugWriteA("Set RAM Bank: %02X\n", Cart->iCurrentRamBankNo);
			if (Cart->bHasTimer && (Data[0] >= 0x08 && Data[0] <= 0x0c)) {
				// Set the bank for the timer
				Cart->iCurrentRamBankNo = Data[0];
			}
		}
		break;
	case 3: //	else if ((dwAddress >= 0x6000) && (dwAddress <= 0x7FFF)) // Latch timer data
		CopyMemory(Cart->LatchedTimerData, Cart->TimerData, 5 * sizeof(Cart->TimerData[0]));
		if (Data[0] & 1) {
			// Update timer, save latch values, and set latch state
			UpdateRTC(Cart);
			for (i=0; i<4; i++)
				Cart->LatchedTimerData[i] = Cart->TimerData[i];
			Cart->TimerDataLatched = true;
			DebugWriteA("Timer Data Latch: Enable\n");
		} else {
			Cart->TimerDataLatched = false;
			DebugWriteA("Timer Data Latch: Disable\n");
		}
		break;
	case 5: //	else if ((dwAddress >= 0xA000) && (dwAddress <= 0xBFFF)) // Write to RAM
		if (Cart->bHasRam) {
			if (Cart->iCurrentRamBankNo >= 0x08 && Cart->iCurrentRamBankNo <= 0x0c) {
				// Write to the timer
				DebugWriteA("Timer write: Bank %02X\n", Cart->iCurrentRamBankNo);
				Cart->TimerData[Cart->iCurrentRamBankNo - 0x08] = Data[0];
			} else {
				DebugWriteA("RAM write: Bank %02X%s\n", Cart->iCurrentRamBankNo, Cart->bRamEnableState ? "" : " -- NOT ENABLED (but wrote anyway)");
				CopyMemory(&Cart->RamData[dwAddress - 0xA000 + (Cart->iCurrentRamBankNo * 0x2000)], Data, 32);
			}
		}
		break;
	default:
		DebugWriteA("Bad write to MBC3 cart, address %04X\n", dwAddress);
	}

	return true;
}

// Done
bool ReadCartMBC5(LPGBCART Cart, WORD dwAddress, BYTE *Data)
{
	switch (dwAddress >> 13)
	{
	case 0:
	case 1: //	if ((dwAddress <= 0x3FFF))
		CopyMemory(Data, &Cart->RomData[dwAddress], 32);
		DebugWriteA("Nonbanked ROM read - MBC5\n");
		break;
	case 2:
	case 3: //	else if ((dwAddress >= 0x4000) && (dwAddress <= 0x7FFF))
		if (Cart->iCurrentRomBankNo >= Cart->iNumRomBanks) {
			ZeroMemory(Data, 32);
			DebugWriteA("Banked ROM read: (Banking Error)");
			DebugWriteByteA(Cart->iCurrentRomBankNo);
			DebugWriteA("\n");
		} else {
			CopyMemory(Data, &Cart->RomData[dwAddress - 0x4000 + (Cart->iCurrentRomBankNo << 14)], 32);
			DebugWriteA("Banked ROM read: Bank=");
			DebugWriteByteA(Cart->iCurrentRomBankNo);
			DebugWriteA("\n");
		}
		break;
	case 5: //	else if ((dwAddress >= 0xA000) && (dwAddress <= 0xBFFF))
		if (Cart->bHasRam) {
			if (Cart->iCurrentRamBankNo >= Cart->iNumRamBanks) {
				ZeroMemory(Data, 32);
				DebugWriteA("Failed RAM read: (Banking Error) %02X\n", Cart->iCurrentRamBankNo);
			} else {
				CopyMemory(Data, &Cart->RamData[dwAddress - 0xA000 + (Cart->iCurrentRamBankNo << 13)], 32);
				DebugWriteA("RAM read: Bank %02X\n", Cart->iCurrentRamBankNo);
			}
		} else {
			ZeroMemory(Data, 32);
			DebugWriteA("Failed RAM read: (RAM Not Present)\n");
		}
		break;
	default:
		DebugWriteA("Bad read from MBC5 cart, address %04X\n", dwAddress);
	}

	return true;
}

// Done
bool WriteCartMBC5(LPGBCART Cart, WORD dwAddress, BYTE *Data)
{
	switch (dwAddress >> 12)
	{
	case 0: //	if ((dwAddress <= 0x1FFF)) // We shouldn't be able to read/write to RAM unless this is toggled on
	case 1:
		Cart->bRamEnableState = (Data[0] == 0x0A);
		DebugWriteA("Set RAM enable: %d\n", Cart->bRamEnableState);
		break;
	case 2: //	else if ((dwAddress >= 0x2000) && (dwAddress <= 0x2FFF)) // ROM bank select, low bits
		Cart->iCurrentRomBankNo &= 0xFF00;
		Cart->iCurrentRomBankNo |= Data[0];
		// Cart->iCurrentRomBankNo = ((int) Data[0]) | (Cart->iCurrentRomBankNo & 0x100);
		DebugWriteA("Set ROM Bank: %02X\n", Cart->iCurrentRomBankNo);
		break;
	case 3: //	else if ((dwAddress >= 0x3000) && (dwAddress <= 0x3FFF)) // ROM bank select, high bit
		Cart->iCurrentRomBankNo &= 0x00FF;
		Cart->iCurrentRomBankNo |= (Data[0] & 0x01) << 8;
		// Cart->iCurrentRomBankNo = (Cart->iCurrentRomBankNo & 0xFF) | ((((int) Data[0]) & 1) * 0x100);
		DebugWriteA("Set ROM Bank: %02X\n", Cart->iCurrentRomBankNo);
		break;
	case 4: //	if ((dwAddress >= 0x4000) && (dwAddress <= 0x5FFF)) // RAM bank select
	case 5:
		if (Cart->bHasRam) {
			Cart->iCurrentRamBankNo = Data[0] & 0x0F;
			DebugWriteA("Set RAM Bank: %02X\n", Cart->iCurrentRamBankNo);
		}
		break;
	case 10: //	else if ((dwAddress >= 0xA000) && (dwAddress <= 0xBFFF)) // Write to RAM
	case 11:
		if (Cart->bHasRam) {
			if (Cart->iCurrentRamBankNo >= Cart->iNumRamBanks) {
				DebugWriteA("RAM write: Buffer error on ");
				DebugWriteByteA(Cart->iCurrentRamBankNo);
				DebugWriteA("\n");
			} else {
				DebugWriteA("RAM write: Bank %02X\n", Cart->iCurrentRamBankNo);
				CopyMemory(&Cart->RamData[dwAddress - 0xA000 + (Cart->iCurrentRamBankNo << 13)], Data, 32);
			}
		}
		break;
	default:
		DebugWriteA("Bad write to MBC5 cart, address %04X\n", dwAddress);
	}

	return true;
}

bool SaveCart(LPGBCART Cart, LPTSTR SaveFile, LPTSTR TimeFile)
{
	DWORD NumQuarterBlocks = 0;
	gbCartRTC RTCTimer;

	if (Cart->sGoombaRamPath != NULL) {
		UpdateGoombaFile(Cart); // Save data is compressed - we have to write the whole file
	} else if(Cart->bHasRam && Cart->bHasBattery) {
		// Write only the bytes that NEED writing!
		switch (Cart->RomData[0x149]) {
		case 1:
			NumQuarterBlocks = 1;
			break;
		case 2:
			NumQuarterBlocks = 4;
			break;
		case 3:
			NumQuarterBlocks = 16;
			break;
		case 4:
			NumQuarterBlocks = 64;
			break;
		}
		FlushViewOfFile( Cart->RamData, NumQuarterBlocks * 0x0800 );
		if (Cart->bHasTimer) {
			// Save RTC in VisualBoy Advance format
			// TODO: Check if VBA saves are compatible with other emus.
			// TODO: Only write RTC data if VBA RTC data was originaly present
			RTCTimer.mapperSeconds = Cart->TimerData[0];
			RTCTimer.mapperMinutes = Cart->TimerData[1];
			RTCTimer.mapperHours = Cart->TimerData[2];
			RTCTimer.mapperDays = Cart->TimerData[3];
			RTCTimer.mapperControl = Cart->TimerData[4];
			RTCTimer.mapperLSeconds = Cart->LatchedTimerData[0];
			RTCTimer.mapperLMinutes = Cart->LatchedTimerData[1];
			RTCTimer.mapperLHours = Cart->LatchedTimerData[2];
			RTCTimer.mapperLDays = Cart->LatchedTimerData[3];
			RTCTimer.mapperLControl = Cart->LatchedTimerData[4];
			RTCTimer.mapperLastTime = Cart->timerLastUpdate;

			CopyMemory(Cart->RamData + NumQuarterBlocks * 0x0800, &RTCTimer, sizeof(RTCTimer));

			FlushViewOfFile( Cart->RamData + NumQuarterBlocks * 0x0800, sizeof(gbCartRTC));
		}
	}
	return true;
}

bool UnloadCart(LPGBCART Cart)
{
	if (Cart->hRomFile != NULL)
	{
		UnmapViewOfFile(Cart->RomData);
		CloseHandle(Cart->hRomFile);
		Cart->hRomFile = NULL;
	}
	else if (Cart->RomData != NULL)
	{
		P_free((LPVOID)(Cart->RomData));
		Cart->RomData = NULL;
	}

	if (Cart->hRamFile != NULL)
	{
		UnmapViewOfFile(Cart->RamData);
		CloseHandle(Cart->hRamFile);
		Cart->hRamFile = NULL;
	}
	else if (Cart->sGoombaRamPath != NULL)
		 {
		free(Cart->sGoombaRamPath);
		Cart->sGoombaRamPath = NULL;
		P_free(Cart->RamData);
		Cart->RamData = NULL;
		}
	else if (Cart->RamData != NULL)
	{
		P_free(Cart->RamData);
		Cart->RamData = NULL;
	}
	return true;
}

// This is used to clear the RAM data to look like it has just been turned on.
// When a RAM chip is first turned on, it is filled with alternating 128-byte
// blocks of 0x00 and 0xFF.
void ClearData(BYTE *Data, int Length)
{
	int i;

	for (i=0; i<Length; i++) {
		if ((i & 0x80) != 0x80) {
			Data[i] = 0x00;
		} else {
			Data[i] = 0xFF;
		}
	}
}
