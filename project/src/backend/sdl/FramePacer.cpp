#include "FramePacer.h"

#ifdef HX_WINDOWS

#include <windows.h>
#include <winuser.h>
#include <wingdi.h>
#include <dwmapi.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <thread>
#include <vector>


namespace lime {


	// Minimal D3DKMT ABI, resolved from gdi32.dll via GetProcAddress
	typedef LONG NTSTATUS;
	typedef ULONG D3DKMT_HANDLE;

	typedef struct {

		LUID AdapterLuid;
		D3DKMT_HANDLE hAdapter;

	} FP_OpenAdapterFromLuid;

	typedef struct {

		D3DKMT_HANDLE hAdapter;

	} FP_CloseAdapter;

	typedef struct {

		D3DKMT_HANDLE hAdapter;
		D3DKMT_HANDLE hDevice;
		UINT VidPnSourceId;

	} FP_WaitForVerticalBlankEvent;

	typedef NTSTATUS (APIENTRY *FP_OpenAdapterFromLuidFunc) (FP_OpenAdapterFromLuid*);
	typedef NTSTATUS (APIENTRY *FP_CloseAdapterFunc) (FP_CloseAdapter*);
	typedef NTSTATUS (APIENTRY *FP_WaitForVerticalBlankEventFunc) (FP_WaitForVerticalBlankEvent*);
	typedef HRESULT (WINAPI *FP_DwmGetCompositionTimingInfoFunc) (HWND, DWM_TIMING_INFO*);


	static const Uint64 ANCHOR_STALE_NS = 200000000ULL;
	static const Uint64 SPIN_NS = 1000000ULL; // SDL sleeps on a high-res waitable timer; 1 ms covers the wake
	static const Uint64 DEFAULT_EPS_NS = 400000ULL;

	// D3DKMT wakes after the edge, so the PLL phase is late by the wake latency;
	// pad the lead when the hardware clock is unavailable
	static const Uint64 PLL_EPS_NS = 600000ULL;
	static const Uint64 PLL_BIAS_NS = 200000ULL;

	// a blocking GPU readback kills the CPU/GPU overlap; this budget lets the
	// frame body fit one grid (LIME_GPU_MARGIN_NS overrides it)
	static const Uint64 DEFAULT_GPU_MARGIN_NS = 2500000ULL;
	static const Uint64 STAT_WINDOW_NS = 10000000000ULL;


	struct FramePacerImpl {

		// gdi32 / dwmapi entry points
		FP_OpenAdapterFromLuidFunc openAdapter;
		FP_CloseAdapterFunc closeAdapter;
		FP_WaitForVerticalBlankEventFunc waitVBlank;
		FP_DwmGetCompositionTimingInfoFunc dwmTiming;
		Uint64 qpcFreq;

		// window / display target
		SDL_Window* sdlWindow;
		HWND hwndValue;
		Uint64 periodNs;
		std::atomic<Uint32> requestGen;
		std::atomic<bool> useDwm;

		// anchor thread output (SDL clock)
		std::atomic<Uint64> anchorEdge;    // last known vblank edge
		std::atomic<Uint64> anchorPeriod;  // hardware period when it is trusted
		std::atomic<bool> anchorHw;        // edge/period came from DWM, not a wake
		std::atomic<Uint64> vbAlive;
		std::atomic<Uint64> predEdge;      // PLL estimate of the last wake edge
		std::atomic<Uint64> realPeriod;
		std::atomic<bool> rebindHint;
		std::atomic<bool> displayBound;
		std::atomic<Uint64> periodOverride;

		// anchor thread state
		std::thread* anchorThread;
		std::atomic<bool> running;
		D3DKMT_HANDLE hAdapter;
		UINT vidPnSourceId;
		bool adapterOpen;
		Uint32 appliedLocal;
		int failStreak;
		int divergeCount;
		int divergeRound;
		Uint64 lastVbForPeriod;
		Uint64 lastRebindNs;
		int hwFailStreak;

		// game thread pacing
		DWORD lastRefreshRate;
		HMONITOR lastMonitor;
		bool fullscreenMode;
		bool prevFullscreenMode;
		bool gateActive;
		Uint64 lastCheckNs;
		Uint64 framePeriod;
		Uint64 frameEps;
		Uint64 targetEdge;
		Uint64 nextTarget;
		int gridStep;
		Uint64 frameStartT0;
		Uint64 readyAt;
		Uint64 presentAt;
		Uint64 lastPresentAt;
		long long presentErr;
		double bodyEst;
		Uint64 bodyRing[64];
		int bodyIndex;
		int bodyCount;
		Uint64 gpuMargin;
		Uint64 pllBias;
		Uint64 frameBase;
		bool prevAnchorHw;
		Uint64 epsFixed;
		bool epsEnv;
		bool pacerOff;
		Uint64 lastTarget;
		bool pacedLogged;

		// stats window
		Uint32 swapCount;
		Uint32 tearCount;
		Uint32 lateCount;
		long long errSum;
		long long errMin;
		long long errMax;
		Uint32 ivRing[256];
		Uint32 ivIndex;
		Uint32 ivCount;
		Uint64 statWindow;


		FramePacerImpl () {

			openAdapter = nullptr;
			closeAdapter = nullptr;
			waitVBlank = nullptr;
			dwmTiming = nullptr;
			qpcFreq = 0;

			sdlWindow = nullptr;
			hwndValue = nullptr;
			periodNs = 0;
			requestGen = 0;
			useDwm = false;

			anchorEdge = 0;
			anchorPeriod = 0;
			anchorHw = false;
			vbAlive = 0;
			predEdge = 0;
			realPeriod = 0;
			rebindHint = false;
			displayBound = false;
			periodOverride = 0;

			anchorThread = nullptr;
			running = false;
			hAdapter = 0;
			vidPnSourceId = 0;
			adapterOpen = false;
			appliedLocal = 0;
			failStreak = 0;
			divergeCount = 0;
			divergeRound = 0;
			lastVbForPeriod = 0;
			lastRebindNs = 0;
			hwFailStreak = 0;

			lastRefreshRate = 0;
			lastMonitor = nullptr;
			fullscreenMode = false;
			prevFullscreenMode = false;
			gateActive = false;
			lastCheckNs = 0;
			framePeriod = 0;
			frameEps = 0;
			targetEdge = 0;
			nextTarget = 0;
			gridStep = 1;
			frameStartT0 = 0;
			readyAt = 0;
			presentAt = 0;
			lastPresentAt = 0;
			presentErr = 0;
			bodyEst = 3000000.0;
			bodyIndex = 0;
			bodyCount = 0;
			gpuMargin = DEFAULT_GPU_MARGIN_NS;
			pllBias = PLL_BIAS_NS;
			frameBase = 0;
			prevAnchorHw = false;
			epsFixed = DEFAULT_EPS_NS;
			epsEnv = false;
			pacerOff = getenv ("LIME_NO_SCANLINE_GATE") != nullptr;
			lastTarget = 0;
			pacedLogged = false;

			swapCount = 0;
			tearCount = 0;
			lateCount = 0;
			errSum = 0;
			errMin = 0;
			errMax = 0;
			ivIndex = 0;
			ivCount = 0;
			statWindow = 0;

			const char* eps = getenv ("LIME_PRESENT_LEAD_NS");

			if (eps) {

				epsFixed = (Uint64) strtoull (eps, nullptr, 10);
				epsEnv = true;

				if (epsFixed < 50000ULL) epsFixed = 50000ULL;
				if (epsFixed > 2000000ULL) epsFixed = 2000000ULL;

			}

			const char* margin = getenv ("LIME_GPU_MARGIN_NS");

			if (margin) {

				gpuMargin = (Uint64) strtoull (margin, nullptr, 10);

				if (gpuMargin > 8000000ULL) gpuMargin = 8000000ULL;

			}

			const char* bias = getenv ("LIME_VBLANK_BIAS_NS");

			if (bias) {

				pllBias = (Uint64) strtoull (bias, nullptr, 10);

				if (pllBias > 2000000ULL) pllBias = 2000000ULL;

			}

			HMODULE gdi = GetModuleHandleW (L"gdi32.dll");

			if (gdi) {

				openAdapter = (FP_OpenAdapterFromLuidFunc) GetProcAddress (gdi, "D3DKMTOpenAdapterFromLuid");
				closeAdapter = (FP_CloseAdapterFunc) GetProcAddress (gdi, "D3DKMTCloseAdapter");
				waitVBlank = (FP_WaitForVerticalBlankEventFunc) GetProcAddress (gdi, "D3DKMTWaitForVerticalBlankEvent");

			}

			HMODULE dwm = LoadLibraryW (L"dwmapi.dll");

			if (dwm) {

				dwmTiming = (FP_DwmGetCompositionTimingInfoFunc) GetProcAddress (dwm, "DwmGetCompositionTimingInfo");

			}

			LARGE_INTEGER freq;

			if (QueryPerformanceFrequency (&freq)) {

				qpcFreq = (Uint64) freq.QuadPart;

			}

			for (int i = 0; i < 64; i++) bodyRing[i] = 0;

			for (int i = 0; i < 256; i++) ivRing[i] = 0;

		}


		~FramePacerImpl () {

			if (anchorThread) {

				delete anchorThread;

			}

		}


		void CloseCurrentAdapter () {

			if (adapterOpen && closeAdapter) {

				FP_CloseAdapter ca;
				ca.hAdapter = hAdapter;
				closeAdapter (&ca);

			}

			adapterOpen = false;
			hAdapter = 0;

		}


		void OnWindowSet (SDL_Window* window) {

			if (sdlWindow) return;

			sdlWindow = window;

			SDL_PropertiesID props = SDL_GetWindowProperties (sdlWindow);
			hwndValue = (HWND) SDL_GetPointerProperty (props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);

			if (!running) {

				running = true;
				anchorThread = new std::thread (AnchorLoopStatic, this);

			}

		}


		// monitor/frequency via Win32/CCD, not SDL's tracking -- same path the
		// anchor uses, so pacing and anchor can never bind different displays
		bool RefreshDisplayTarget () {

			if (!sdlWindow) return false;

			HMONITOR monitor = MonitorFromWindow (hwndValue, MONITOR_DEFAULTTONEAREST);

			if (!monitor) return false;

			MONITORINFOEXW mi;

			mi.cbSize = sizeof (MONITORINFOEXW);

			if (!GetMonitorInfoW (monitor, (LPMONITORINFO) &mi)) return false;

			DEVMODEW dm;

			dm.dmSize = sizeof (DEVMODEW);

			if (!EnumDisplaySettingsW (mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return false;

			if (!(dm.dmFields & DM_DISPLAYFREQUENCY) || dm.dmDisplayFrequency < 20 || dm.dmDisplayFrequency > 1000) return false;

			// DWM timing only describes the primary display
			POINT origin = { 0, 0 };
			HMONITOR primary = MonitorFromPoint (origin, MONITOR_DEFAULTTOPRIMARY);

			useDwm = primary && monitor == primary;

			// g-sync wobble must not wipe the period override; only a display move does
			if (monitor != lastMonitor) {

				periodOverride = 0;

			}

			lastMonitor = monitor;
			lastRefreshRate = dm.dmDisplayFrequency;
			periodNs = (Uint64) std::llround (1e9 / (double) dm.dmDisplayFrequency);
			requestGen++;
			displayBound = true;

			ResetPacing ();

			char logBuf[192];

			snprintf (logBuf, sizeof (logBuf), "[FramePacer] display bound: %ls freq=%lu Hz dwm=%d\n",
				mi.szDevice, (unsigned long) dm.dmDisplayFrequency, useDwm.load () ? 1 : 0);
			GateLogRaw (logBuf);

			return true;

		}


		// Reopen the anchor when the window moves to another display or the mode changes
		void CheckDisplay () {

			if (!sdlWindow) return;

			// a dead HWND resolves monitors against an empty rect forever
			if (!IsWindow (hwndValue)) {

				SDL_PropertiesID props = SDL_GetWindowProperties (sdlWindow);
				HWND fresh = (HWND) SDL_GetPointerProperty (props, SDL_PROP_WINDOW_WIN32_HWND_POINTER, nullptr);

				if (fresh && fresh != hwndValue) {

					hwndValue = fresh;
					lastMonitor = nullptr;
					RefreshDisplayTarget ();
					return;

				}

			}

			// anchor/CCD disagree: one side bound the wrong display (throttled)
			if (rebindHint.exchange (false)) {

				Uint64 now = SDL_GetTicksNS ();

				if (!lastRebindNs || now - lastRebindNs > 1000000000ULL) {

					lastRebindNs = now;
					lastMonitor = nullptr;
					RefreshDisplayTarget ();

				}

				return;

			}

			HMONITOR monitor = MonitorFromWindow (hwndValue, MONITOR_DEFAULTTONEAREST);

			if (monitor != lastMonitor) {

				RefreshDisplayTarget ();
				return;

			}

			// detect driver-panel mode changes via CCD; with a period override active, wobble is noise
			if (lastRefreshRate && !periodOverride.load ()) {

				MONITORINFOEXW mi;

				mi.cbSize = sizeof (MONITORINFOEXW);

				if (GetMonitorInfoW (monitor, (LPMONITORINFO) &mi)) {

					DEVMODEW dm;

					dm.dmSize = sizeof (DEVMODEW);

					if (EnumDisplaySettingsW (mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)
						&& (dm.dmFields & DM_DISPLAYFREQUENCY)
						&& dm.dmDisplayFrequency != lastRefreshRate) {

						RefreshDisplayTarget ();

					}

				}

			}

		}


		// Resolve the window's monitor to (adapter, VidPnSourceId) and open it
		bool RequeryTarget () {

			CloseCurrentAdapter ();

			if (!openAdapter || !hwndValue) return false;

			HMONITOR monitor = MonitorFromWindow (hwndValue, MONITOR_DEFAULTTONEAREST);

			if (!monitor) return false;

			MONITORINFOEXW mi;

			mi.cbSize = sizeof (MONITORINFOEXW);

			if (!GetMonitorInfoW (monitor, (LPMONITORINFO) &mi)) return false;

			UINT32 pathCount = 0, modeCount = 0;

			if (GetDisplayConfigBufferSizes (QDC_ONLY_ACTIVE_PATHS, &pathCount, &modeCount) != ERROR_SUCCESS) return false;

			std::vector<DISPLAYCONFIG_PATH_INFO> paths (pathCount);
			std::vector<DISPLAYCONFIG_MODE_INFO> modes (modeCount);

			if (QueryDisplayConfig (QDC_ONLY_ACTIVE_PATHS, &pathCount, paths.data (), &modeCount, modes.data (), nullptr) != ERROR_SUCCESS) return false;

			for (UINT32 i = 0; i < pathCount; i++) {

				DISPLAYCONFIG_SOURCE_DEVICE_NAME sourceName;

				sourceName.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
				sourceName.header.size = sizeof (DISPLAYCONFIG_SOURCE_DEVICE_NAME);
				sourceName.header.adapterId = paths[i].sourceInfo.adapterId;
				sourceName.header.id = paths[i].sourceInfo.id;

				if (DisplayConfigGetDeviceInfo (&sourceName.header) != ERROR_SUCCESS) continue;

				if (wcscmp (sourceName.viewGdiDeviceName, mi.szDevice) == 0) {

					FP_OpenAdapterFromLuid oa;

					oa.AdapterLuid = paths[i].sourceInfo.adapterId;

					if (openAdapter (&oa) >= 0) {

						hAdapter = oa.hAdapter;
						vidPnSourceId = paths[i].sourceInfo.id;
						adapterOpen = true;
						lastVbForPeriod = 0;
						failStreak = 0;
						hwFailStreak = 0;

						// stale interval would trip the divergence check
						realPeriod = 0;
						divergeCount = 0;
						divergeRound = 0;

						char logBuf[128];

						snprintf (logBuf, sizeof (logBuf),
							"[FramePacer] anchor ready: %ls VidPn=%u\n",
							mi.szDevice, (unsigned) vidPnSourceId);
						GateLogRaw (logBuf);

						return true;

					}

					return false;

				}

			}

			return false;

		}


		Uint64 QpcToNs (Uint64 ticks) {

			if (!qpcFreq) return 0;

			return (Uint64) ((double) ticks * 1000000000.0 / (double) qpcFreq);

		}


		// Waits real vblank interrupts and publishes a predicted edge clock
		void AnchorLoop () {

			while (running) {

				// wait for the first stable binding or the anchor latches the birth screen
				if (!displayBound.load ()) {

					for (int i = 0; i < 20 && running; i++) SDL_DelayNS (50000000ULL);
					continue;

				}

				Uint32 req = requestGen;

				if (req != appliedLocal || !adapterOpen) {

					if (RequeryTarget ()) {

						appliedLocal = req;

					} else {

						LogState ("anchor requery failed");

						for (int i = 0; i < 100 && running; i++) SDL_DelayNS (50000000ULL);
						continue;

					}

				}

				FP_WaitForVerticalBlankEvent wait;

				wait.hAdapter = hAdapter;
				wait.hDevice = 0;
				wait.VidPnSourceId = vidPnSourceId;

				NTSTATUS status = waitVBlank (&wait);

				if (status < 0) {

					if (++failStreak >= 8) {

						CloseCurrentAdapter ();

						for (int i = 0; i < 20 && running; i++) SDL_DelayNS (50000000ULL);

					}

					continue;

				}

				failStreak = 0;

				Uint64 now = SDL_GetTicksNS ();

				vbAlive = now;

				// Smooth the measured interval, rejecting outliers (VRR, wakeups)
				if (lastVbForPeriod) {

					Uint64 delta = now - lastVbForPeriod;

					if (delta > 100000ULL && delta < 200000000ULL) {

						Uint64 current = realPeriod.load ();

						if (!current) {

							realPeriod = delta;

						} else if (std::llabs ((long long) delta - (long long) current) < (long long) (current / 4)) {

							// signed math: unsigned underflowed and stuck realPeriod at ~2^61 ns
							realPeriod = (Uint64) ((long long) current + ((long long) delta - (long long) current) / 8);

						}

					}

				}

				lastVbForPeriod = now;

				Uint64 measured = realPeriod.load ();
				Uint64 basePeriod = measured ? measured : periodNs;

				// PLL on wake times: the absolute phase is late by the wake latency,
				// the jitter is what the filter is for
				if (basePeriod) {

					Uint64 pred = predEdge.load ();

					if (!pred) {

						pred = now;

					} else {

						pred += basePeriod;

						long long err = (long long) now - (long long) pred;

						pred = (Uint64) ((long long) pred + err / 8);

					}

					predEdge = pred;

				}

				// hardware edge, no thread wake bias
				bool hwOk = false;
				Uint64 hwEdge = 0;
				Uint64 hwPeriod = 0;

				if (useDwm.load () && dwmTiming && qpcFreq) {

					DWM_TIMING_INFO ti;

					memset (&ti, 0, sizeof (ti));
					ti.cbSize = (UINT) sizeof (ti);

					if (dwmTiming (nullptr, &ti) == S_OK && ti.qpcVBlank) {

						LARGE_INTEGER qpcNow;

						QueryPerformanceCounter (&qpcNow);

						long long delta = (long long) qpcNow.QuadPart - (long long) ti.qpcVBlank;

						if (delta >= 0) {

							Uint64 age = QpcToNs ((Uint64) delta);

							if (age < 100000000ULL) {

								hwOk = true;
								hwEdge = now - age;
								hwPeriod = ti.qpcRefreshPeriod ? QpcToNs ((Uint64) ti.qpcRefreshPeriod) : 0;

							}

						}

					}

				}

				if (hwOk) {

					hwFailStreak = 0;
					anchorEdge = hwEdge;

					if (hwPeriod && (!periodNs || (hwPeriod > periodNs / 2 && hwPeriod < periodNs * 2))) {

						anchorPeriod = hwPeriod;

					} else {

						anchorPeriod = basePeriod;

					}

					anchorHw = true;

				} else if (anchorHw.load () && ++hwFailStreak < 20 && now - anchorEdge.load () < 200000000ULL) {

					// one stale DWM sample must not flip the phase back to the late wake clock

				} else {

					hwFailStreak = 0;
					anchorEdge = predEdge.load ();
					anchorPeriod = basePeriod;
					anchorHw = false;

				}

				// persistent measured-vs-CCD mismatch = wrong display binding
				Uint64 base = periodNs;
				Uint64 smoothed = realPeriod.load ();

				if (base >= 1000000ULL && smoothed) {

					if (smoothed < base / 4 * 3 || smoothed > base / 4 * 5) {

						if (++divergeCount >= 4) {

							divergeCount = 0;

							// round 2+: rebind hints loop forever (g-sync) -- trust the hardware value
							if (++divergeRound == 1) {

								rebindHint = true;

								char dBuf[128];

								snprintf (dBuf, sizeof (dBuf),
									"[FramePacer] diverge: meas=%.2f ms ccd=%.2f ms\n",
									smoothed / 1e6, base / 1e6);
								GateLogRaw (dBuf);

							} else if (divergeRound == 2) {

								periodOverride = smoothed;

								char oBuf[128];

								snprintf (oBuf, sizeof (oBuf),
									"[FramePacer] period override: using measured %.2f ms\n",
									smoothed / 1e6);
								GateLogRaw (oBuf);

							} else {

								periodOverride = smoothed;

							}

						}

					} else {

						divergeCount = 0;

						if (divergeRound) divergeRound = 0;

					}

				}

			}

		}


		static void AnchorLoopStatic (FramePacerImpl* pacer) {

			// anchor timestamps feed every phase calculation -- keep this thread fast
			SetThreadPriority (GetCurrentThread (), THREAD_PRIORITY_TIME_CRITICAL);

			pacer->AnchorLoop ();

		}


		// Measured period when sane, SDL display mode value otherwise
		Uint64 EffectivePeriod () {

			Uint64 override = periodOverride.load ();

			if (override >= 1000000ULL && override <= 100000000ULL) return override;

			Uint64 p = realPeriod.load ();
			Uint64 base = periodNs;

			if (base < 1000000ULL) return 0;

			if (p < base * 7 / 10 || p > base * 13 / 10) p = base;

			return p;

		}


		// DWM's period is exact; the measured one is only used as a fallback
		Uint64 PacingPeriod () {

			Uint64 p = EffectivePeriod ();

			if (anchorHw.load ()) {

				Uint64 hp = anchorPeriod.load ();

				if (hp && (!periodNs || (hp > periodNs * 9 / 10 && hp < periodNs * 11 / 10))) p = hp;

			}

			return p;

		}


		Uint64 EffectiveEps () {

			if (epsEnv) return epsFixed;

			return anchorHw.load () ? epsFixed : (epsFixed > PLL_EPS_NS ? epsFixed : PLL_EPS_NS);

		}


		// First edge strictly after t, computed from the anchor's last edge
		Uint64 NextEdgeAfter (Uint64 t, Uint64 period, Uint64 base) {

			if (!period || !base) return 0;

			Uint64 e = base;

			if (t > base) e = base + ((t - base) / period) * period;

			if (e <= t) e += period;

			return e;

		}


		// cadence state is per-display; must not survive a monitor switch
		void ResetPacing () {

			gateActive = false;
			framePeriod = 0;
			frameEps = 0;
			targetEdge = 0;
			nextTarget = 0;
			gridStep = 1;
			frameStartT0 = 0;
			readyAt = 0;
			presentAt = 0;
			lastPresentAt = 0;
			presentErr = 0;
			bodyEst = 3000000.0;
			bodyIndex = 0;
			bodyCount = 0;
			frameBase = 0;
			pacedLogged = false;

			swapCount = 0;
			tearCount = 0;
			lateCount = 0;
			errSum = 0;
			errMin = 0;
			errMax = 0;
			ivIndex = 0;
			ivCount = 0;
			statWindow = 0;

		}


		// stderr/OutputDebugString are invisible for a GUI app -- log to file too
		void GateLogRaw (const char* text) {

			static std::mutex logMutex;
			std::lock_guard<std::mutex> lock (logMutex);

			char stamped[256];

			snprintf (stamped, sizeof (stamped), "[%llu ms] %s",
				(unsigned long long) (SDL_GetTicksNS () / 1000000ULL), text);

			OutputDebugStringA (stamped);
			fprintf (stderr, "%s", stamped);

			static FILE* logFile = nullptr;

			if (!logFile) {

				logFile = fopen ("framepacer.log", "a");

			}

			if (logFile) {

				fputs (stamped, logFile);
				fflush (logFile);

			}

		}


		void LogState (const char* event) {

			char buf[160];

			Uint64 now = SDL_GetTicksNS ();
			Uint64 alive = vbAlive.load ();
			bool fresh = alive && now - alive <= ANCHOR_STALE_NS;

			snprintf (buf, sizeof (buf), "[FramePacer] %s | anchor=%s P=%.3f ms\n",
				event, fresh ? "live" : "stale", EffectivePeriod () / 1e6);

			GateLogRaw (buf);

		}


		void LogPaced () {

			if (pacedLogged) return;

			pacedLogged = true;

			char buf[224];

			snprintf (buf, sizeof (buf),
				"[FramePacer] paced: anchor=%s P=%.3f ms eps=%.3f ms margin=%.3f ms step=%d\n",
				anchorHw.load () ? "hw" : "pll",
				framePeriod / 1e6,
				frameEps / 1e6,
				gpuMargin / 1e6,
				gridStep);

			GateLogRaw (buf);

		}


		// log on state change or every 10 s; intervals, not swap counts, are the evidence
		void LogStateHeartbeat () {

			Uint64 now = SDL_GetTicksNS ();

			if (!statWindow) {

				statWindow = now;
				return;

			}

			if (now - statWindow < STAT_WINDOW_NS) return;

			double secs = (double) (now - statWindow) / 1e9;
			double fps = secs > 0.0 ? (double) swapCount / secs : 0.0;
			double avgErr = swapCount ? (double) errSum / (double) swapCount / 1e6 : 0.0;

			double tgt = lastTarget ? 1e9 / (double) lastTarget : 0.0;

			Uint32 p50 = 0, p99 = 0, ivMax = 0;

			if (ivCount) {

				Uint32 tmp[256];

				for (Uint32 i = 0; i < ivCount; i++) tmp[i] = ivRing[i];

				for (Uint32 i = 1; i < ivCount; i++) {

					Uint32 v = tmp[i];
					Uint32 j = i;

					while (j > 0 && tmp[j - 1] > v) {

						tmp[j] = tmp[j - 1];
						j--;

					}

					tmp[j] = v;

				}

				p50 = tmp[ivCount / 2];
				p99 = tmp[(Uint32) ((double) (ivCount - 1) * 0.99)];
				ivMax = tmp[ivCount - 1];

			}

			char buf[384];

			snprintf (buf, sizeof (buf),
				"[FramePacer] state | fs=%d tgt=%.0f step=%d anchor=%s P=%.3f eps=%.3f body=%.2f margin=%.2f | fps=%.1f swaps=%u tear=%u late=%u err avg=%.2f min=%.2f max=%.2f | iv50=%.2f iv99=%.2f ivmax=%.2f\n",
				fullscreenMode ? 1 : 0,
				tgt,
				gridStep,
				anchorHw.load () ? "hw" : "pll",
				framePeriod / 1e6,
				frameEps / 1e6,
				bodyEst / 1e6,
				gpuMargin / 1e6,
				fps,
				swapCount,
				tearCount,
				lateCount,
				avgErr,
				errMin / 1e6,
				errMax / 1e6,
				p50 / 1e6,
				p99 / 1e6,
				ivMax / 1e6);

			GateLogRaw (buf);

			swapCount = 0;
			tearCount = 0;
			lateCount = 0;
			errSum = 0;
			errMin = 0;
			errMax = 0;
			ivIndex = 0;
			ivCount = 0;
			statWindow = now;

		}


		bool WaitFrameStart (Uint64 target) {

			lastTarget = target;

			bool fs = sdlWindow && (SDL_GetWindowFlags (sdlWindow) & SDL_WINDOW_FULLSCREEN) != 0;

			if (fs != prevFullscreenMode) {

				prevFullscreenMode = fs;
				ResetPacing ();

			}

			fullscreenMode = fs;

			LogStateHeartbeat ();

			Uint64 now = SDL_GetTicksNS ();

			// display checks run even when pacing is off (3 s poll, first bind retries every frame)
			if (!lastMonitor || rebindHint.load () || now - lastCheckNs >= 3000000000ULL) {

				lastCheckNs = now;
				CheckDisplay ();

			}

			gateActive = false;

			if (!sdlWindow || !openAdapter || !waitVBlank || pacerOff) return false;

			Uint64 alive = vbAlive.load ();

			if (!alive || now - alive > ANCHOR_STALE_NS) return false;

			// windowed: DWM decides when the frame is latched, the time-based cap owns the rate
			if (!fullscreenMode) return false;

			// Never pace on top of vsync
			int swapInterval = 0;

			if (SDL_GL_GetSwapInterval (&swapInterval) && swapInterval != 0) return false;

			Uint64 period = PacingPeriod ();

			if (!period) return false;

			Uint64 base = anchorEdge.load ();

			if (!base) return false;

			// cap below the refresh rate: present every whole number of refreshes
			int step = 1;

			if (target != 0) {

				double ratio = (double) target / (double) period;

				step = (int) std::llround (ratio);

				if (step < 1) step = 1;
				if (step > 8) step = 8;

			}

			bool hw = anchorHw.load ();

			if (hw != prevAnchorHw) {

				prevAnchorHw = hw;
				nextTarget = 0;

			}

			// D3DKMT wakes after the edge; shift the predicted clock by the mean wake
			if (!hw && base > pllBias) base -= pllBias;

			gridStep = step;
			framePeriod = period;
			frameBase = base;

			Uint64 grid = period * (Uint64) step;
			Uint64 edge;

			if (!nextTarget || nextTarget <= now) {

				edge = NextEdgeAfter (now, grid, base);

			} else {

				edge = nextTarget;

			}

			if (!edge) return false;

			targetEdge = edge;

			Uint64 eps = EffectiveEps ();

			if (!epsEnv) {

				// body over one grid: the extra lead is the overlap the next frame
				// needs to start before this frame's GPU tail finishes
				double over = bodyEst + 100000.0 - (double) grid;

				if (over > 0.0) {

					Uint64 autoEps = (Uint64) over;

					if (autoEps > eps) eps = autoEps;

				}

				if (eps > 3000000ULL) eps = 3000000ULL;

			}

			frameEps = eps;

			Uint64 lead = (Uint64) bodyEst + eps;

			if (lead < 300000ULL) lead = 300000ULL;
			if (lead > grid + eps) lead = grid + eps;

			// start early enough for the body to end eps before the edge; never
			// start before the previous present, that would be two frames in flight
			Uint64 start = (edge > lead) ? edge - lead : 0;

			if (start < lastPresentAt) start = lastPresentAt;
			if (start < now) start = now;

			if (start > now) {

				Uint64 wait = start - now;

				if (wait > SPIN_NS) SDL_DelayNS (wait - SPIN_NS);

				while (SDL_GetTicksNS () < start) {

				}

			}

			frameStartT0 = SDL_GetTicksNS ();
			gateActive = true;

			LogPaced ();

			return true;

		}


		bool ShouldSwap (Uint64 now) {

			if (!gateActive) return true;

			// present just before the edge: an on-time flip tears at the bottom
			// (or not at all), a post-edge flip costs the whole frame
			Uint64 deadline = (targetEdge > frameEps) ? targetEdge - frameEps : targetEdge;

			readyAt = SDL_GetTicksNS ();

			if (readyAt < deadline) {

				Uint64 wait = deadline - readyAt;

				if (wait > SPIN_NS) SDL_DelayNS (wait - SPIN_NS);

				while (SDL_GetTicksNS () < deadline) {

				}

			}

			presentAt = SDL_GetTicksNS ();
			presentErr = (long long) presentAt - (long long) targetEdge;

			if (presentErr > 0) tearCount++;
			else if (readyAt > deadline) lateCount++;

			if (lastPresentAt) {

				Uint64 iv = presentAt - lastPresentAt;

				if (iv > 0 && iv < 1000000000ULL) {

					ivRing[ivIndex] = (Uint32) iv;
					ivIndex = (ivIndex + 1) & 255;

					if (ivCount < 256) ivCount++;

				}

			}

			lastPresentAt = presentAt;

			errSum += presentErr;

			if (swapCount == 0) {

				errMin = presentErr;
				errMax = presentErr;

			} else {

				if (presentErr < errMin) errMin = presentErr;
				if (presentErr > errMax) errMax = presentErr;

			}

			swapCount++;

			return true;

		}



		double BodyPercentile (double p) {

			if (!bodyCount) return 0.0;

			Uint64 tmp[64];

			for (int i = 0; i < bodyCount; i++) tmp[i] = bodyRing[i];

			for (int i = 1; i < bodyCount; i++) {

				Uint64 v = tmp[i];
				int j = i - 1;

				while (j >= 0 && tmp[j] > v) {

					tmp[j + 1] = tmp[j];
					j--;

				}

				tmp[j + 1] = v;

			}

			int idx = (int) (p * (double) (bodyCount - 1) + 0.5);

			if (idx < 0) idx = 0;
			if (idx >= bodyCount) idx = bodyCount - 1;

			return (double) tmp[idx];

		}


		void OnFrameEnd (Uint64 frameEnd) {

			if (!gateActive || !readyAt || readyAt < frameStartT0) return;

			Uint64 cpu = readyAt - frameStartT0;

			bodyRing[bodyIndex] = cpu;
			bodyIndex = (bodyIndex + 1) & 63;

			if (bodyCount < 64) bodyCount++;

			bodyEst = BodyPercentile (0.90) + (double) gpuMargin;

			if (framePeriod) {

				double cap = (double) framePeriod * (double) gridStep + 3000000.0;

				if (bodyEst > cap) bodyEst = cap;

			}

			if (bodyEst < 300000.0) bodyEst = 300000.0;

			// one present per grid point: the next target is the one after this
			// edge, and a late frame re-anchors on the edge after the present
			Uint64 grid = framePeriod * (Uint64) gridStep;

			if (grid) {

				if (presentErr <= 0) nextTarget = targetEdge + grid;
				else nextTarget = NextEdgeAfter (presentAt, grid, frameBase);

			} else {

				nextTarget = 0;

			}

		}

	};


	FramePacer::FramePacer () {

		impl = nullptr;

	}


	FramePacer::~FramePacer () {

		if (impl) {

			FramePacerImpl* pacer = (FramePacerImpl*) impl;

			pacer->running = false;

			if (pacer->anchorThread && pacer->anchorThread->joinable ()) {

				pacer->anchorThread->join ();

			}

			pacer->CloseCurrentAdapter ();
			delete pacer;

			impl = nullptr;

		}

	}


	void FramePacer::SetWindow (SDL_Window* sdlWindow) {

		if (!impl) {

			FramePacerImpl* pacer = new FramePacerImpl ();

			if (!pacer->openAdapter || !pacer->closeAdapter || !pacer->waitVBlank) {

				delete pacer;
				return;

			}

			impl = pacer;

		}

		((FramePacerImpl*) impl)->OnWindowSet (sdlWindow);

	}


	bool FramePacer::WaitFrameStart (Uint64 target) {

		if (!impl) return false;

		return ((FramePacerImpl*) impl)->WaitFrameStart (target);

	}


	bool FramePacer::ShouldSwap (Uint64 now) {

		if (!impl) return true;

		return ((FramePacerImpl*) impl)->ShouldSwap (now);

	}



	void FramePacer::OnFrameEnd (Uint64 frameEnd) {

		if (impl) ((FramePacerImpl*) impl)->OnFrameEnd (frameEnd);

	}


}


#else // HX_WINDOWS


namespace lime {


	FramePacer::FramePacer () {

		impl = nullptr;

	}


	FramePacer::~FramePacer () {


	}


	void FramePacer::SetWindow (SDL_Window* sdlWindow) {


	}


	bool FramePacer::WaitFrameStart (Uint64 target) {

		return false;

	}


	bool FramePacer::ShouldSwap (Uint64 now) {

		return true;

	}



	void FramePacer::OnFrameEnd (Uint64 frameEnd) {


	}


}


#endif // HX_WINDOWS
