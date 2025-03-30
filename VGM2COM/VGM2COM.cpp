/*
https://www.fpga4fun.com/OPL.html

VGM2COM player
It streams VGM YM3812 (OPL2) data to a COM port.

Usage:
VGM2COM VGMfilename COMport

For example
VGM2COM ergo.vgm COM4

The COMport parameter is optional... if omitted, the player uses COM3 by default.

Compiles with Visual Studio 2022 C++
*/

#include <conio.h>
#include <iostream>
#include <fstream>
#include <io.h>

#include <windows.h>
#include <sysinfoapi.h>

const WCHAR* defaultCOMport = L"COM3";
const int maxplaytime = 999;  // will stop after that many seconds

HANDLE hCom;
void OpenCom(LPCWSTR COM_name)
{
	DCB dcb;
	COMMTIMEOUTS ct;
	hCom = CreateFile(COM_name, GENERIC_READ | GENERIC_WRITE, 0, NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
	if (hCom == INVALID_HANDLE_VALUE) throw std::exception("Can't open the COM port");
	if (!SetupComm(hCom, 4096, 4096)) throw std::exception("SetupComm error");
	if (!GetCommState(hCom, &dcb)) throw std::exception("GetCommState error");
	
	dcb.BaudRate = 115200;
	((DWORD*)(&dcb))[2] = 0x1001; // set port properties for TXDI + no flow-control
	dcb.ByteSize = 8;
	dcb.Parity = NOPARITY;
	dcb.StopBits = 2;
	if (!SetCommState(hCom, &dcb)) throw std::exception("SetCommState error");
	
	// set the timeouts to 0
	ct.ReadIntervalTimeout = MAXDWORD;
	ct.ReadTotalTimeoutMultiplier = 0;
	ct.ReadTotalTimeoutConstant = 0;
	ct.WriteTotalTimeoutMultiplier = 0;
	ct.WriteTotalTimeoutConstant = 0;
	if (!SetCommTimeouts(hCom, &ct)) throw std::exception("SetCommTimeouts error");
}

void CloseCom()
{
	CloseHandle(hCom);
}

void WriteCom(char* buf, int len)
{
	DWORD nSend;
	if (!WriteFile(hCom, buf, len, &nSend, NULL) || nSend != len) throw std::exception("Can't send COM data");
}

// we use the GetSystemTimeAsFileTime() function for good time accuracy (0.1us)
// but the VGM defines time as "samples" = 1/44100 of a second = 22.7us
// so we' use'll need a conversion factor to go from one to the other
const int VGM2FT = 227;  // ratio between FileTime and VGMsamples time spaces

LONGLONG FTnow()
{
	LONGLONG FT;
	GetSystemTimeAsFileTime((LPFILETIME)&FT);
	return FT;
}

// we use a memory buffer for the COM data
char OPL_COMbuf[0x200];
int OPL_COMbufi;

bool KeepPlaying;
LONGLONG FTtime;

void waitsamples(int nSamples)
{
	// we send the buffered OPL data to the COM port
	WriteCom(OPL_COMbuf, OPL_COMbufi);
	OPL_COMbufi = 0;

	// we wait for the requested VGM samples time to elapse
	FTtime += nSamples * VGM2FT;
	while (FTnow() - FTtime < 0) Sleep(1);

	// and now the optional stuff...
	static int fracsamples, playtime;
	fracsamples += nSamples;
	if (fracsamples >= 44100) {
		std::cout << ".";  // print a dot every second
		fracsamples -= 44100;
		playtime++;
		KeepPlaying = (playtime < maxplaytime) && (!_kbhit() || _getch()!=27);
	}
}

void Play(char* p, char* pend)
{
	FTtime = FTnow();
	while (KeepPlaying && p < pend)
	{
		switch (*p)
		{
		case 0x5A:  // YM3812
		{
			p++;
			OPL_COMbuf[OPL_COMbufi++] = *p++;  // YM3812 address
			OPL_COMbuf[OPL_COMbufi++] = *p++;  // YM3812 data
			if (OPL_COMbufi >= sizeof(OPL_COMbuf)) waitsamples(0);  // and flush if full
			break;
		}
		case 0x61:
		{
			waitsamples(*PWORD(++p));
			p += 2;
			break;
		}
		case 0x62:
		{
			waitsamples(735);
			p++;
			break;
		}
		case 0x63:
		{
			waitsamples(882);
			p++;
			break;
		}
		case 0x66:  // end of song
			return;
		case 0x70:
		case 0x71:
		case 0x72:
		case 0x73:
		case 0x74:
		case 0x75:
		case 0x76:
		case 0x77:
		case 0x78:
		case 0x79:
		case 0x7A:
		case 0x7B:
		case 0x7C:
		case 0x7D:
		case 0x7E:
		case 0x7F:
		{
			waitsamples((*p++ & 0xF) + 1);
			break;
		}
		default:
 continue;
		//	throw std::exception("Unknown VGM tag... Is it an OPL2 VGM?");	// unknown tag... end playback
		}
	}
}

int wmain(int argc, wchar_t* argv[])
{
	if (argc == 2 || argc == 3)
	{
		const WCHAR* filename = argv[1];
		const WCHAR* COMport = (argc == 3) ? argv[2] : defaultCOMport;

		std::wcout << "Streaming \"" << filename << "\" on " << COMport;
		try
		{
			FILE* F;
			if (_wfopen_s(&F, filename, L"rb") == 0)
			{
				long filesize = _filelength(_fileno(F));	if (filesize == INVALID_FILE_SIZE) throw std::exception("Invalid file length");
				char* pbuf = (char*)malloc(filesize);		if (!pbuf) throw std::exception("unable to allocate memory");
				size_t read = fread(pbuf, 1, filesize, F);		if (read != filesize) throw std::exception("Unable to read the full file");
				fclose(F);
				if (pbuf[0] != 'V' || pbuf[1] != 'g' || pbuf[2] != 'm' || pbuf[3] != ' ') throw std::exception("Not a VGM file");
				if (pbuf[9] != 1 || pbuf[8] < 51) throw std::exception("File needs to be a VGM 1.51 or later");

				// find where the music starts and ends
				int sb = *PINT(pbuf + 0x34);
				char* pstart = pbuf + (sb == 0 ? 0x40 : sb + 0x34);
				char* pend = pbuf + filesize;

				KeepPlaying = true;
				OpenCom(COMport);
				Play(pstart, pend);
				CloseCom();	

				std::cout << (KeepPlaying ? " done\n" : " stopped\n");
				free(pbuf);
			}
			else
				std::wcout << "... Unable to open \"" << filename << "\" file\n";
		}
		catch (const std::exception& e) 
		{
			std::cout << "... ERROR! " << e.what() << "\n";
		}
	}
	else
	{
		std::cout << "VGM2COM OPL2 player 1.00 (c) fpga4fun.com & KNJN LLC 2024\n";
		std::cout << "Need one or two arguments... one mandatory filename and one optional COMport\n";
	}

	Sleep(1000);
	return 0;
}
