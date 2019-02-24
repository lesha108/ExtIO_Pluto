
#define HWNAME				"Pluto"
#define HWMODEL				"Pluto"
#define VERNUM              "0.01"
#define SETTINGS_IDENTIFIER	"Pluto-1.x"
#define LO_MIN				70000000LL
#define LO_MAX				6000000000LL
#define EXT_BLOCKLEN		512 * 4		/* only multiples of 512 */

#include "iio.h"
#include "ExtIO_Pluto.h"

//---------------------------------------------------------------------------
#include <windows.h>
#include <windowsx.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include "resource.h"

//---------------------------------------------------------------------------

#ifdef _DEBUG
#define _MYDEBUG // Activate a debug console
#endif

#ifdef  _MYDEBUG
/* Debug Trace Enabled */
#define DbgPrintf printf
#else
/* Debug Trace Disabled */
#define DbgPrintf(Message) MessageBoxA(NULL, Message, NULL, MB_OK|MB_ICONERROR) 
#endif

#define MHZ(x) ((long long)(x*1000000.0 + .5))
#define GHZ(x) ((long long)(x*1000000000.0 + .5))

static char gSDR[1025] = "ip:192.168.2.1\0";

/* RX is input, TX is output */
enum iodev { RX, TX };

/* common RX and TX streaming params */
struct stream_cfg {
	long long bw_hz; // Analog banwidth in Hz
	long long fs_hz; // Baseband sample rate in Hz
	long long lo_hz; // Local oscillator frequency in Hz
	const char* rfport; // Port name
};

/* IIO structs required for streaming */
static struct iio_context *ctx = NULL;
static struct iio_channel *rx0_i = NULL;
static struct iio_channel *rx0_q = NULL;
static struct iio_buffer  *rxbuf = NULL;

// Streaming devices
static struct iio_device *rx = NULL;

// Stream configurations
static struct stream_cfg rxcfg;

#pragma warning(disable : 4996)

#define snprintf	_snprintf

static bool SDR_supports_settings = false;  // assume not supported
static bool SDR_settings_valid = false;		// assume settings are for some other ExtIO

static char SDR_progname[32+1] = "\0";
static int  SDR_ver_major = -1;
static int  SDR_ver_minor = -1;

static int		gHwType = exthwUSBdata16; // 16bit UINT
static int		giExtSrateIdx = 0;
static unsigned gExtSampleRate = 2500000; // just default

volatile int64_t	glLOfreq = 0L;
bool	gbInitHW = false;

pfnExtIOCallback	pfnCallback = 0;
volatile bool	gbExitThread = false;
volatile bool	gbThreadRunning = false;

///---
EXTERN_C IMAGE_DOS_HEADER __ImageBase;
HWND h_dialog = nullptr;
HANDLE thread_handle = INVALID_HANDLE_VALUE;

//---------------------------------------------------------------------------
void UpdateDialog()
{
	SetDlgItemTextA(h_dialog, IDC_ALPF_BW, gSDR);
}

//---------------------------------------------------------------------------
static INT_PTR CALLBACK MainDlgProc(HWND hwndDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{

	switch (uMsg) {

	case WM_INITDIALOG:
	{
		UpdateDialog();
		return TRUE;
	}
	break;

	case WM_COMMAND:
	{
		switch (GET_WM_COMMAND_ID(wParam, lParam)) {
		case IDC_BUTTON_CAl:
		{
			if (GET_WM_COMMAND_CMD(wParam, lParam) == BN_CLICKED) {

				if (gbThreadRunning)
				{
					MessageBoxA(NULL, "Stop receiver first!", "Error", MB_OK | MB_ICONEXCLAMATION);
					return TRUE;
				}

				int buffSize = GetWindowTextLength(GetDlgItem(hwndDlg, IDC_ALPF_BW));
				char *textBuffer = new char[buffSize + 1];

				GetDlgItemTextA(hwndDlg, IDC_ALPF_BW, textBuffer, buffSize + 1);

				strcpy(gSDR, textBuffer);
				free(textBuffer);

				UpdateDialog();

				if (ctx) { iio_context_destroy(ctx); };
				ctx = iio_create_context_from_uri(gSDR);
				if (ctx == NULL) {
					gbInitHW = false;
					MessageBoxA(NULL, "Connection failed!", "Error", MB_OK | MB_ICONERROR);
				}
				else
				{
					gbInitHW = true;
					MessageBoxA(NULL, "Connection successful!", "Info", MB_OK | MB_ICONINFORMATION);
				};

				return TRUE;
			}
		}
		break;
		}
	}
	break;

	case WM_SHOWWINDOW:
		UpdateDialog();
		return TRUE;
		break;

	case WM_CLOSE:
		ShowWindow(h_dialog, SW_HIDE);
		return TRUE;
		break;

	case WM_DESTROY:
		ShowWindow(h_dialog, SW_HIDE);
		h_dialog = NULL;
		return TRUE;
		break;
	}
	return FALSE;
}

//---------------------------------------------------------------------------
DWORD WINAPI GeneratorThreadProc( __in  LPVOID lpParameter )
{
	int16_t	iqbuf[EXT_BLOCKLEN * 2];
	ssize_t	nbytes_rx;
	char	*p_dat, *p_end;
	ptrdiff_t	p_inc;

	int	iqcnt = 0; // pointer to sample in iqbuf
	
	while ( !gbExitThread )
	{
		nbytes_rx = iio_buffer_refill(rxbuf);
		//if (nbytes_rx < 0) { printf("Error refilling buf %d\n", (int)nbytes_rx); }
		p_inc = iio_buffer_step(rxbuf);
		p_end = (char *)iio_buffer_end(rxbuf);
		for (p_dat = (char *)iio_buffer_first(rxbuf, rx0_i); p_dat < p_end; p_dat += p_inc) {

			iqbuf[iqcnt++] = ((int16_t*)p_dat)[0];
			iqbuf[iqcnt++] = ((int16_t*)p_dat)[1];

			if (iqcnt == EXT_BLOCKLEN * 2) { // buffer full
				iqcnt = 0;
				pfnCallback(EXT_BLOCKLEN, 0, 0.0F, &iqbuf[0]);
			}
		}
	}
	gbExitThread = false;
	gbThreadRunning = false;
	return 0;
}

static void stopThread()
{
	if ( gbThreadRunning )
	{
		gbExitThread = true;
		while ( gbThreadRunning )
		{
			SleepEx( 10, FALSE );
		}
	}
}

static void startThread()
{
	gbExitThread = false;
	gbThreadRunning = true;

	thread_handle = CreateThread( NULL	// LPSECURITY_ATTRIBUTES lpThreadAttributes
		, (SIZE_T)(64 * 1024)	// SIZE_T dwStackSize
		, GeneratorThreadProc	// LPTHREAD_START_ROUTINE lpStartAddress
		, NULL					// LPVOID lpParameter
		, 0						// DWORD dwCreationFlags
		, NULL					// LPDWORD lpThreadId
		);
	SetThreadPriority(thread_handle, THREAD_PRIORITY_TIME_CRITICAL);
}

//---------------------------------------------------------------------------
BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved )
{
	switch (ul_reason_for_call)
	{
	case DLL_PROCESS_ATTACH:
	case DLL_THREAD_ATTACH:
	case DLL_THREAD_DETACH:
	case DLL_PROCESS_DETACH:
		break;
	}
	return TRUE;
}

//---------------------------------------------------------------------------
extern "C"
bool __declspec(dllexport) __stdcall InitHW(char *name, char *model, int& type)
{
	/* Create debug console window */
#ifdef _MYDEBUG
	if (AllocConsole()) {
		FILE* f;
		freopen_s(&f, "CONOUT$", "wt", stdout);
		SetConsoleTitle(TEXT("Debug Console ExtIO_Pluto " VERNUM));
		SetConsoleTextAttribute(GetStdHandle(STD_OUTPUT_HANDLE), FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_RED);
	}
#endif

	type = gHwType;
	strcpy(name,  HWNAME);
	strcpy(model, HWMODEL);

	if ( !gbInitHW )
	{
		ctx = iio_create_context_from_uri(gSDR);
		if (ctx == NULL) {
			MessageBoxA(NULL, "Connection failed!", "Error", MB_OK | MB_ICONERROR);
		};

		glLOfreq = LO_MIN;	// just a default value
		gbInitHW = true;
	}
	return true;
}

//---------------------------------------------------------------------------
extern "C"
bool EXTIO_API OpenHW(void)
{
	h_dialog = CreateDialog((HINSTANCE)&__ImageBase, MAKEINTRESOURCE(ExtioDialog), NULL, (DLGPROC)MainDlgProc);
	ShowWindow(h_dialog, SW_HIDE);
	return true;
}

//---------------------------------------------------------------------------
extern "C"
int  EXTIO_API StartHW(long LOfreq)
{
	int64_t ret = StartHW64( (int64_t)LOfreq );
	return (int)ret;
}

//---------------------------------------------------------------------------
extern "C"
int64_t EXTIO_API StartHW64(int64_t LOfreq)
{
	struct iio_channel *chn = NULL;

	//DbgPrintf("StartHW64\n");
	if (!gbInitHW)
		return 0;

	stopThread();

	// AD IIO init

	// RX stream config
	rxcfg.bw_hz = (long)(gExtSampleRate * 0.8); // MHZ(2);   // 2 MHz rf bandwidth
	rxcfg.fs_hz = gExtSampleRate; // MHZ(2.5);   // 2.5 MS/s rx sample rate
	rxcfg.lo_hz = LOfreq; // MHZ(145.5); // 2.5 GHz rf frequency
	rxcfg.rfport = "A_BALANCED"; // port A (select for rf freq.)

#ifdef _MYDEBUG
	printf("SDR addr: %s\n", gSDR);
	printf("* Acquiring IIO context\n");
#endif
	if (ctx) { iio_context_destroy(ctx); };
	ctx = iio_create_context_from_uri(gSDR);
	if (ctx == NULL) {
		MessageBoxA(NULL, "Connection failed!", "Error", MB_OK | MB_ICONERROR);
		return 0;
	};
#ifdef _MYDEBUG
	if (ctx) {
		printf("Devices count %u \n", iio_context_get_devices_count(ctx));
	};
#endif
	rx = iio_context_find_device(ctx, "cf-ad9361-lpc");
	if (rx == NULL) {
		DbgPrintf("rx not created\n");
		return 0;
	};
#ifdef _MYDEBUG
	if (rx) {
		printf("RX device found\n");
	};
#endif
	// setting reciever
	chn = iio_device_find_channel(iio_context_find_device(ctx, "ad9361-phy"), "voltage0", false);
	if (chn == NULL) {
		DbgPrintf("chn not created\n");
		return 0;
	};
#ifdef _MYDEBUG
	if (chn) {
		printf("chn device found\n");
	};
#endif
	if (iio_channel_attr_write(chn, "rf_port_select", rxcfg.rfport) < 0) {
		DbgPrintf("rf_port_select failed\n");
	};
	if (iio_channel_attr_write_longlong(chn, "rf_bandwidth", rxcfg.bw_hz) < 0) {
		DbgPrintf("rf_bandwidth failed\n");
	};
	if (iio_channel_attr_write_longlong(chn, "sampling_frequency", rxcfg.fs_hz) < 0) {
		DbgPrintf("sampling_frequency failed\n");
	};

	// setting LO
	chn = iio_device_find_channel(iio_context_find_device(ctx, "ad9361-phy"), "altvoltage0", true);
	if (chn == NULL) {
		DbgPrintf("chnLO not created\n");
		return 0;
	};
#ifdef _MYDEBUG
	if (chn) {
		printf("chnLO device found\n");
	};
#endif
	if (iio_channel_attr_write_longlong(chn, "frequency", rxcfg.lo_hz) < 0) {
		DbgPrintf("frequency set failed\n");
	};

	// Initializing AD9361 IIO streaming channels
	rx0_i = iio_device_find_channel(rx, "voltage0", false);
	if (rx0_i == NULL) {
		DbgPrintf("rx0_i not created\n");
		return 0;
	};
#ifdef _MYDEBUG
	if (rx0_i) {
		printf("rx0_i device found\n");
	};
#endif
	rx0_q = iio_device_find_channel(rx, "voltage1", false);
	if (rx0_q == NULL) {
		DbgPrintf("rx0_q not created\n");
		return 0;
	};
#ifdef _MYDEBUG
	if (rx0_q) {
		printf("rx0_q device found\n");
	};
#endif
	// Enabling IIO streaming channels
	iio_channel_enable(rx0_i);
	iio_channel_enable(rx0_q);

	rxbuf = iio_device_create_buffer(rx, 1024 * 16, false);
	if (!rxbuf) {
		DbgPrintf("Could not create RX buffer");
		if (rx0_i) { iio_channel_disable(rx0_i); }
		if (rx0_q) { iio_channel_disable(rx0_q); }
		return 0;
	}
#ifdef _MYDEBUG
	else {
		printf("rxbuf created\n");
	}
#endif
	startThread();

	// number of complex elements returned each
	// invocation of the callback routine
	return EXT_BLOCKLEN;
}

//---------------------------------------------------------------------------
extern "C"
void EXTIO_API StopHW(void)
{
	//DbgPrintf("StopHW\n");

	stopThread();

	if (rxbuf) { iio_buffer_destroy(rxbuf); }
	if (rx0_i) { iio_channel_disable(rx0_i); }
	if (rx0_q) { iio_channel_disable(rx0_q); }

	return;  // nothing to do with this specific HW
}

//---------------------------------------------------------------------------
extern "C"
void EXTIO_API CloseHW(void)
{
//	DbgPrintf("CloseHW\n");
	if (gbInitHW )
	{
		if (ctx) { iio_context_destroy(ctx); }
	}
	DestroyWindow(h_dialog);

	gbInitHW = false;
}

//---------------------------------------------------------------------------
extern "C"
int  EXTIO_API SetHWLO(long LOfreq)
{
	int64_t ret = SetHWLO64( (int64_t)LOfreq );
	return (ret & 0xFFFFFFFF);
}

extern "C"
int64_t EXTIO_API SetHWLO64(int64_t LOfreq)
{
//	DbgPrintf("SetHWLO64\n");
	const int64_t wishedLO = LOfreq;
	int64_t ret = 0;

	// check limits
	if ( LOfreq < LO_MIN )
	{
		LOfreq = LO_MIN;
		ret = -LO_MIN;
	}
	else if ( LOfreq > LO_MAX )
	{
		LOfreq = LO_MAX;
		ret = LO_MAX;
	}

	// take frequency
	glLOfreq = LOfreq;

	if ( gbInitHW && ctx )
	{
		struct iio_channel *chn = NULL;

		// setting LO
		chn = iio_device_find_channel(iio_context_find_device(ctx, "ad9361-phy"), "altvoltage0", true);
		if (chn == NULL) {
			DbgPrintf("chnLO not created\n");
			return 0;
		};
#ifdef _MYDEBUG
		if (chn) {
			printf("chnlo device found\n");
		};
#endif
		if (iio_channel_attr_write_longlong(chn, "frequency", glLOfreq) < 0) {
			DbgPrintf("frequency set failed\n");
		};
	}

	if ( wishedLO != LOfreq  &&  pfnCallback )
		pfnCallback( -1, extHw_Changed_LO, 0.0F, 0 );

	// 0 The function did complete without errors.
	// < 0 (a negative number N)
	//     The specified frequency  is  lower than the minimum that the hardware  is capable to generate.
	//     The absolute value of N indicates what is the minimum supported by the HW.
	// > 0 (a positive number N) The specified frequency is greater than the maximum that the hardware
	//     is capable to generate.
	//     The value of N indicates what is the maximum supported by the HW.
	return ret;
}

//---------------------------------------------------------------------------
extern "C"
int  EXTIO_API GetStatus(void)
{
	return 0;  
}

//---------------------------------------------------------------------------
extern "C"
void EXTIO_API SetCallback( pfnExtIOCallback funcptr )
{
	pfnCallback = funcptr;
	return;
}

//---------------------------------------------------------------------------
extern "C"
long EXTIO_API GetHWLO(void)
{
	int64_t	glLOfreq1 = GetHWLO64();
	return (long)( glLOfreq1 & 0xFFFFFFFF );
}

extern "C"
int64_t EXTIO_API GetHWLO64(void)
{
	if (gbInitHW && ctx)
	{
		struct iio_channel *chn = NULL;
		long long frq = 0;

		// setting LO
		chn = iio_device_find_channel(iio_context_find_device(ctx, "ad9361-phy"), "altvoltage0", true);
		if (chn == NULL) {
			DbgPrintf("chnLO not created\n");
			return 0;
		};
#ifdef _MYDEBUG
		if (chn) {
			printf("chnlo device found\n");
		};
#endif
		if (iio_channel_attr_read_longlong(chn, "frequency", &frq) < 0) {
			DbgPrintf("frequency read failed\n");
		};
		glLOfreq = frq;
	}
	return glLOfreq;
}

//---------------------------------------------------------------------------
extern "C"
long EXTIO_API GetHWSR(void)
{
	//DbgPrintf("GetHWSR\n");
	// This DLL controls just an oscillator, not a digitizer
	return gExtSampleRate;
}

//---------------------------------------------------------------------------
extern "C"
void EXTIO_API VersionInfo(const char * progname, int ver_major, int ver_minor)
{
  SDR_progname[0] = 0;
  SDR_ver_major = -1;
  SDR_ver_minor = -1;

  if ( progname )
  {
    strncpy( SDR_progname, progname, sizeof(SDR_progname) -1 );
    SDR_ver_major = ver_major;
    SDR_ver_minor = ver_minor;
  }
}

//---------------------------------------------------------------------------
extern "C"
int EXTIO_API ExtIoGetSrates( int srate_idx, double * samplerate )
{
	switch ( srate_idx )
	{
	case 0:		*samplerate = 2500000.0;	return 0;
	case 1:		*samplerate = 3000000.0;	return 0;
	case 2:		*samplerate = 4000000.0;	return 0;
	case 3:		*samplerate = 6000000.0;	return 0;
	case 4:		*samplerate = 10000000.0;	return 0;
	case 5:		*samplerate = 20000000.0;	return 0;
	default:	return 1;	// ERROR
	}
	return 1;	// ERROR
}

extern "C"
int  EXTIO_API ExtIoGetActualSrateIdx(void)
{
	return giExtSrateIdx;
}

extern "C"
int  EXTIO_API ExtIoSetSrate( int srate_idx )
{
	double newSrate = 0.0;
	if ( 0 == ExtIoGetSrates( srate_idx, &newSrate ) )
	{
		giExtSrateIdx = srate_idx;

		gExtSampleRate = (unsigned)( newSrate + 0.5 );
		rxcfg.fs_hz = gExtSampleRate; 
		rxcfg.bw_hz = (long)(gExtSampleRate * 0.8);

// setting reciever samplerate
		if (ctx) {
			struct iio_channel *chn = NULL;

			chn = iio_device_find_channel(iio_context_find_device(ctx, "ad9361-phy"), "voltage0", false);
			if (chn == NULL) {
				DbgPrintf("chn not created\n");
				return 1;
			};
#ifdef _MYDEBUG
			if (chn) {
				printf("chn device found\n");
			};
#endif
			if (iio_channel_attr_write_longlong(chn, "rf_bandwidth", rxcfg.bw_hz) < 0) {
				DbgPrintf("rf_bandwidth set failed\n");
				return 1;
			};
			if (iio_channel_attr_write_longlong(chn, "sampling_frequency", rxcfg.fs_hz) < 0) {
				DbgPrintf("sampling_frequency set failed\n");
				return 1;
			};
		}
		else return 1;

		return 0;
	}
	return 1;	// ERROR
}

extern "C"
long EXTIO_API ExtIoGetBandwidth( int srate_idx )
{
	double newSrate = 0.0;
	long ret = -1L;
	if ( 0 == ExtIoGetSrates( srate_idx, &newSrate ) )
	{
		switch ( srate_idx )
		{
			case 0:		
			case 1:		
			case 2:		
			case 3:		
			case 4:		
			case 5:		ret = (long)(newSrate * 0.8);	break;
			default:	ret = -1L;		break;
		}
		return ( ret >= newSrate || ret <= 0L ) ? -1L : ret;
	}
	return -1L;	// ERROR
}

//---------------------------------------------------------------------------

extern "C"
int  EXTIO_API ExtIoGetSetting( int idx, char * description, char * value )
{
	switch ( idx )
	{
	case 0: snprintf( description, 1024, "%s", "Identifier" );		snprintf( value, 1024, "%s", SETTINGS_IDENTIFIER );	return 0;
	case 1:	snprintf( description, 1024, "%s", "SampleRateIdx" );	snprintf( value, 1024, "%d", giExtSrateIdx );		return 0;
	case 2:	snprintf( description, 1024, "%s", "SDR");		        snprintf( value, 1024, "%s", gSDR);	return 0;
	default:	return -1;	// ERROR
	}
	return -1;	// ERROR
}

extern "C"
void EXTIO_API ExtIoSetSetting( int idx, const char * value )
{
	double newSrate;
	float  newAtten = 0.0F;
	int tempInt;

	SDR_supports_settings = true;
	if ( idx != 0 && !SDR_settings_valid )
		return;	// ignore settings for some other ExtIO

	switch ( idx )
	{
	case 0:		SDR_settings_valid = ( value && !strcmp( value, SETTINGS_IDENTIFIER ) );
				break;
	case 1:		tempInt = atoi( value );
				if ( 0 == ExtIoGetSrates( tempInt, &newSrate ) )
				{
					giExtSrateIdx = tempInt;
					gExtSampleRate = (unsigned)( newSrate + 0.5 );
				}
				break;
	case 2:		strcpy(gSDR, value);
				break;
	}

}

//---------------------------------------------------------------------------
extern "C"
void EXTIO_API ShowGUI()
{
	ShowWindow(h_dialog, SW_SHOW);
	return;
}
//---------------------------------------------------------------------------
extern "C"
void EXTIO_API HideGUI()
{
	ShowWindow(h_dialog, SW_HIDE);
	return;
}
//---------------------------------------------------------------------------
