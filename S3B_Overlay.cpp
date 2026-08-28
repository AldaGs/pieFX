/*
	S3B_Overlay.cpp - throwaway .exe for Phase 0, spike S3B.

	The same layered ring as S3A, in a SEPARATE PROCESS. That difference is the
	entire point: the planned architecture puts the overlay in Tauri, which is
	another process, and two things about S3A's pass do not automatically carry
	over.

	  - A TOPMOST window owned by a background process is not obviously the same
	    z-order fight as one owned by the foreground application.
	  - In-process, AE stayed the active APPLICATION the whole time. Out of
	    process it gets deactivated outright (WM_ACTIVATEAPP), which is a
	    stronger event than the one S3A proved selection survives.

	Deliberately dumb: no window class cleanup, no error recovery, no message
	loop niceties. It draws, it waits, it dies. The plug-in does the measuring.

	Build (from this directory, any Developer prompt):
	    cl /nologo /EHsc /DUNICODE /D_UNICODE S3B_Overlay.cpp /link /SUBSYSTEM:WINDOWS \
	       user32.lib gdi32.lib /OUT:RadialMenu_S3B.exe

	Usage: RadialMenu_S3B.exe <centre_x> <centre_y> <size> <milliseconds>
*/

#include <windows.h>
#include <math.h>
#include <stdlib.h>

static LRESULT CALLBACK
WndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
	if (msg == WM_DESTROY) {
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProc(hwnd, msg, wParam, lParam);
}

//	Same ring as S3A, in a different colour so the two spikes can never be
//	confused for one another on screen. Premultiplied BGRA, top-down.
static void
PaintBits(void *bitsPV, int size)
{
	unsigned int	*px		= (unsigned int*)bitsPV;
	const double	centre	= (size - 1) / 2.0;
	const double	outer	= centre - 6.0;
	const double	inner	= outer - 26.0;

	for (int y = 0; y < size; y++) {
		for (int x = 0; x < size; x++) {
			double	dx	= x - centre;
			double	dy	= y - centre;
			double	d	= sqrt(dx * dx + dy * dy);

			double	a	= 1.0;
			if (d > outer)		a = outer + 1.0 - d;
			else if (d < inner)	a = d - (inner - 1.0);

			if (a < 0.0) a = 0.0;
			if (a > 1.0) a = 1.0;
			a *= 0.85;

			unsigned char A = (unsigned char)(a * 255.0 + 0.5);
			unsigned char R = (unsigned char)(a *  40.0 + 0.5);
			unsigned char G = (unsigned char)(a * 200.0 + 0.5);
			unsigned char B = (unsigned char)(a * 255.0 + 0.5);	// cyan, vs S3A's orange

			px[y * size + x] = (A << 24) | (R << 16) | (G << 8) | B;
		}
	}
}

int WINAPI
WinMain(HINSTANCE inst, HINSTANCE prev, LPSTR cmdline, int show)
{
	int	cx		= (__argc > 1) ? atoi(__argv[1]) : 400;
	int	cy		= (__argc > 2) ? atoi(__argv[2]) : 400;
	int	size	= (__argc > 3) ? atoi(__argv[3]) : 320;
	int	ms		= (__argc > 4) ? atoi(__argv[4]) : 3000;

	WNDCLASSEX	wc;
	ZeroMemory(&wc, sizeof(wc));
	wc.cbSize			= sizeof(wc);
	wc.lpfnWndProc		= WndProc;
	wc.hInstance		= inst;
	wc.hCursor			= LoadCursor(NULL, IDC_ARROW);
	wc.lpszClassName	= TEXT("RadialMenuS3BOverlay");
	RegisterClassEx(&wc);

	int	x = cx - size / 2;
	int	y = cy - size / 2;

	HWND hwnd = CreateWindowEx(
		WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
		TEXT("RadialMenuS3BOverlay"), TEXT(""), WS_POPUP,
		x, y, size, size,
		NULL, NULL, inst, NULL);

	if (!hwnd) {
		return 1;
	}

	BITMAPINFO	bmi;
	ZeroMemory(&bmi, sizeof(bmi));
	bmi.bmiHeader.biSize		= sizeof(BITMAPINFOHEADER);
	bmi.bmiHeader.biWidth		= size;
	bmi.bmiHeader.biHeight		= -size;
	bmi.bmiHeader.biPlanes		= 1;
	bmi.bmiHeader.biBitCount	= 32;
	bmi.bmiHeader.biCompression	= BI_RGB;

	void	*bitsPV	= NULL;
	HDC		screen	= GetDC(NULL);
	HDC		mem		= CreateCompatibleDC(screen);
	HBITMAP	dib		= CreateDIBSection(screen, &bmi, DIB_RGB_COLORS, &bitsPV, NULL, 0);

	if (dib && bitsPV) {
		PaintBits(bitsPV, size);

		HGDIOBJ	old		= SelectObject(mem, dib);
		POINT	dstPt	= { x, y };
		POINT	srcPt	= { 0, 0 };
		SIZE	wndSize	= { size, size };
		BLENDFUNCTION blend;
		blend.BlendOp				= AC_SRC_OVER;
		blend.BlendFlags			= 0;
		blend.SourceConstantAlpha	= 255;
		blend.AlphaFormat			= AC_SRC_ALPHA;

		UpdateLayeredWindow(hwnd, screen, &dstPt, &wndSize,
							mem, &srcPt, 0, &blend, ULW_ALPHA);

		SelectObject(mem, old);
	}

	ShowWindow(hwnd, SW_SHOW);

	//	Take focus ON PURPOSE, exactly as S3A did - this is the whole experiment.
	//	Here it also deactivates AE as an application, which S3A never did.
	SetForegroundWindow(hwnd);

	MessageBeep(MB_OK);

	//	Pump messages so the window is real and paints, but do not outstay.
	DWORD	end = GetTickCount() + (DWORD)ms;
	MSG		msg;

	while (GetTickCount() < end) {
		while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
			if (msg.message == WM_QUIT) {
				goto done;
			}
			TranslateMessage(&msg);
			DispatchMessage(&msg);
		}
		Sleep(10);
	}

done:
	if (dib)	DeleteObject(dib);
	if (mem)	DeleteDC(mem);
	ReleaseDC(NULL, screen);
	DestroyWindow(hwnd);
	return 0;
}
