#include "FramePacer.h"

#ifdef HX_WINDOWS

#include <windows.h>
#include <winuser.h>
#include <wingdi.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
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


	static const Uint64 ANCHOR_STALE_NS = 200000000ULL; // degrade after 200 ms without vblank
	static const Uint64 SPIN_MARGIN_NS = 1000000ULL;    // busy-wait margin, as in SDLApplication

	static const double EPS_MIN = 600000.0;    // submission lead time bounds
	static const double EPS_MAX = 4000000.0;
	static const double EPS_START = 1500000.0;

	static const long long EDGE_MARGIN_NS = 800000LL; // fullscreen: body finish lands this far before the edge (covers the anchor publication delay)

	// scanline gate (fullscreen): present inside the blanking interval; windowed
	// submits pre-boundary (DWM composites on its own schedule)
	static const Uint64 GATE_SLEEP_EDGE_NS = 2500000ULL;    // busy-poll the anchor publication within this window before the edge


	struct FramePacerImpl {

		// gdi32 entry points
		FP_OpenAdapterFromLuidFunc openAdapter;
		FP_CloseAdapterFunc closeAdapter;
		FP_WaitForVerticalBlankEventFunc waitVBlank;

		// window / display target (game thread writes, anchor reads)
		SDL_Window* sdlWindow;
		HWND hwndValue;
		Uint64 periodNs;
		std::atomic<Uint32> requestGen;

		// anchor thread output
		std::atomic<Uint64> vbLast;      // QPC time of the last real vblank
		std::atomic<Uint64> vbAlive;     // anchor heartbeat
		std::atomic<Uint64> realPeriod;  // measured vblank interval (smoothed)
		std::atomic<bool> rebindHint;    // anchor->game: measured period disagrees with the CCD mode
		std::atomic<bool> displayBound;  // game->anchor: first stable CCD resolution done
		std::atomic<Uint64> periodOverride;  // anchor->game: CCD reading proven wrong, use hardware-measured period

		// anchor thread state
		std::thread* anchorThread;
		std::atomic<bool> running;
		D3DKMT_HANDLE hAdapter;
		UINT vidPnSourceId;
		bool adapterOpen;
		Uint32 appliedLocal;
		int failStreak;
		int divergeCount;                // anchor thread: consecutive period mismatches
		int divergeRound;                // anchor thread: mismatch rounds AFTER a forced rebind
		Uint64 lastVbForPeriod;
		Uint64 lastRebindNs;             // game thread: cooldown between rebind-hint consumptions

		// game thread pacing state
		DWORD lastRefreshRate;           // CCD-reported frequency of the bound display
		HMONITOR lastMonitor;
		bool fullscreenMode;             // SDL_WINDOW_FULLSCREEN: scanline gate vs DWM-friendly submission
		Uint64 swapWaitNs;               // game thread: edge-gate wait accumulated this frame
		Uint64 lastCheckNs;              // game thread: last display-environment poll
		Uint64 nextBoundary;
		Uint64 frameStartT0;
		double rEst;                     // smoothed frame-body duration
		double eps;                      // submission lead time (adaptive)
		int missRing[32];                // recent late-submission history
		int missIndex;
		int noMissRun;
		bool gateActive;                 // gated submission (unlimited or cap above refresh)
		Uint64 lastPresented;            // gate mode: last presented boundary
		Uint32 swapCount;                // game thread: presentations since last heartbeat
		Uint32 gateHits;                 // game thread: raster polls that caught the edge
		Uint32 gateMisses;               // game thread: raster polls that missed it
		Uint64 lastGateStart;            // gate mode: render loop cap anchor
		bool scanlineDisabled;           // LIME_NO_SCANLINE_GATE escape hatch
		bool gateFirstHit;               // one-shot log: first successful edge poll
		Uint64 lastGateHash;             // last-logged state signature
		Uint64 lastGateLogTime;          // throttle for state logs
		int gridK;                       // windowed: boundaries per presentation (owned by gridN)
		bool fullscreenTear;             // LIME_NO_FULLSCREEN_TEAR pin: false reverts to the edge grid even over budget
		bool tearActive;                 // live over-budget verdict (0.9P in / 0.8P out hysteresis)
		bool freeRun;                    // fullscreenTear && tearActive: the single free-run verdict ALL consumers share
		bool overloadLogged;             // one-shot log per free-run episode
		bool probeFrame;                 // free-run probe: this frame runs the full edge grid to test re-entry
		int probePasses;                 // consecutive probe frames that caught their edge (2 = leave free-run)
		Uint64 lastProbeNs;              // probe scheduling (one probe per second while free-running)
		Uint64 probeEdge;                // grid point the probe frame aligned its head to (verdict measured against THIS)
		int deepMissStreak;              // consecutive deep misses (4 in a row = the grid cannot hold the body)
		int gridN;                       // whole-number refresh divider: presentations every gridN periods
		int gridMisses;                  // gear window: missed presentations
		int gridSamples;                 // gear window: total presentations
		Uint64 gridWindowStart;          // gear window start (2 s, swappy-style)
		Uint64 lastGridChangeNs;         // cooldown between gear changes
		double filteredBody;             // median-filtered frame body (spike-proof)
		Uint64 bodyRing[5];              // ring feeding the body median
		int bodyIndex;
		bool prevFullscreenMode;         // present-path flip detection (one grid re-anchor)
		Uint64 frameSerial;              // increments once per WaitFrameStart
		Uint64 lastGridSampleFrame;      // frame already counted into the gear window
		Uint64 lastTarget;               // cap target passed to WaitFrameStart (0 = unlimited)
		double capCarry;                 // low-cap Bresenham: fractional boundary-step carry (mean rate = cap)
		Uint64 capGoalBoundary;          // low-cap Bresenham: this frame's goal boundary on the edge grid
		int slowBurst;                   // consecutive slow frames (raw evidence log)
		Uint64 lastSlowLogNs;            // slow-frame log throttle


		FramePacerImpl () {

			openAdapter = nullptr;
			closeAdapter = nullptr;
			waitVBlank = nullptr;

			sdlWindow = nullptr;
			hwndValue = nullptr;
			periodNs = 0;
			requestGen = 0;

			vbLast = 0;
			vbAlive = 0;
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

			lastRefreshRate = 0;
			lastMonitor = nullptr;
			fullscreenMode = false;
			swapWaitNs = 0;
			lastCheckNs = 0;
			nextBoundary = 0;
			frameStartT0 = 0;
			rEst = 0.0;
			eps = EPS_START;
			missIndex = 0;
			noMissRun = 0;
			gateActive = false;
			lastPresented = 0;
			swapCount = 0;
			gateHits = 0;
			gateMisses = 0;
			lastGateStart = 0;
			scanlineDisabled = getenv ("LIME_NO_SCANLINE_GATE") != nullptr;
			gateFirstHit = false;
			lastGateHash = 0;
			lastGateLogTime = 0;
			gridK = 1;
			fullscreenTear = getenv ("LIME_NO_FULLSCREEN_TEAR") == nullptr;
			tearActive = false;
			freeRun = false;
			overloadLogged = false;
			probeFrame = false;
			probePasses = 0;
			lastProbeNs = 0;
			probeEdge = 0;
			deepMissStreak = 0;
			gridN = 1;
			gridMisses = 0;
			gridSamples = 0;
			gridWindowStart = 0;
			lastGridChangeNs = 0;
			filteredBody = 0.0;
			bodyIndex = 0;
			prevFullscreenMode = false;
			frameSerial = 0;
			lastGridSampleFrame = 0;
			lastTarget = 0;
			capCarry = 0.0;
			capGoalBoundary = 0;
			slowBurst = 0;
			lastSlowLogNs = 0;

			for (int i = 0; i < 32; i++) missRing[i] = 0;

			for (int i = 0; i < 5; i++) bodyRing[i] = 0;

			HMODULE gdi = GetModuleHandleW (L"gdi32.dll");

			if (gdi) {

				openAdapter = (FP_OpenAdapterFromLuidFunc) GetProcAddress (gdi, "D3DKMTOpenAdapterFromLuid");
				closeAdapter = (FP_CloseAdapterFunc) GetProcAddress (gdi, "D3DKMTCloseAdapter");
				waitVBlank = (FP_WaitForVerticalBlankEventFunc) GetProcAddress (gdi, "D3DKMTWaitForVerticalBlankEvent");

			}

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

			// window rect isn't final at creation -- CheckDisplay does the first stable binding

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

			// g-sync wobble must not wipe the period override; only a display move does
			if (monitor != lastMonitor) {

				periodOverride = 0;

				// cadence state is per-display, must not survive a switch
				ResetCadenceState ();

			}

			lastMonitor = monitor;
			lastRefreshRate = dm.dmDisplayFrequency;
			periodNs = (Uint64) std::llround (1e9 / (double) dm.dmDisplayFrequency);
			requestGen++;
			displayBound = true;

			// new period invalidates the submission grid anchor
			lastPresented = 0;

			char logBuf[128];

			snprintf (logBuf, sizeof (logBuf), "[FramePacer] display bound: %ls freq=%lu Hz\n",
				mi.szDevice, (unsigned long) dm.dmDisplayFrequency);
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


		// Waits real vblank interrupts and publishes timestamps for extrapolation
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
				vbLast = now;
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


		// cadence state is per-display; must not survive a monitor switch
		void ResetCadenceState () {

			filteredBody = 0.0;
			rEst = 0.0;
			tearActive = false;
			freeRun = false;
			overloadLogged = false;
			probeFrame = false;
			probePasses = 0;
			lastProbeNs = 0;
			probeEdge = 0;
			deepMissStreak = 0;
			gridN = 1;
			gridK = 1;
			lastPresented = 0;
			nextBoundary = 0;
			capCarry = 0.0;
			capGoalBoundary = 0;
			lastGateStart = 0;
			gridWindowStart = 0;
			gridSamples = 0;
			gridMisses = 0;
			lastGridChangeNs = 0;
			slowBurst = 0;

			for (int i = 0; i < 5; i++) bodyRing[i] = 0;

		}


		// swappy-style divider: climb on misses, step down only after a clean window
		void UpdateGridN (bool missed) {

			// free-run owns the presentation policy: freeze the gear while it lasts
			if (fullscreenMode && freeRun) return;

			// a frame can be sampled twice (ShouldSwap + RecordMiss): count it once
			if (lastGridSampleFrame == frameSerial) return;

			lastGridSampleFrame = frameSerial;

			Uint64 period = EffectivePeriod ();

			if (!period) return;

			Uint64 now = SDL_GetTicksNS ();

			if (!gridWindowStart) gridWindowStart = now;

			gridSamples++;

			if (missed) gridMisses++;

			// 2 s window or 64 frames, whichever first
			if (now - gridWindowStart < 2000000000ULL && gridSamples < 64) return;

			int pct = gridSamples ? gridMisses * 100 / gridSamples : 0;

			int wantN = gridN;

			// body can't fit the grid: smallest whole divider keeps intervals even
			// if (pct > 10 && filteredBody > (double) period * gridN) {
			//
			// 	wantN = (int) std::ceil (filteredBody / (double) period);
			//
			// 	if (wantN > 8) wantN = 8;
			//
			// clean window that fits the next-lower grid: give the rate back
			// } else
			if (pct == 0 && gridN > 1
				&& filteredBody < (double) period * (gridN - 1) - 500000.0) {

				wantN = gridN - 1;

			}

			if (wantN != gridN && now - lastGridChangeNs > 100000000ULL) {

				gridN = wantN;
				lastGridChangeNs = now;

				// phase re-anchors on the new grid, no catch-up
				lastPresented = 0;
				nextBoundary = 0;

				char b[160];

				snprintf (b, sizeof (b),
					"[FramePacer] grid -> N=%d (miss=%d%% body=%.2f ms)\n",
					gridN, pct, filteredBody / 1e6);
				GateLogRaw (b);

			}

			// fresh window either way: old samples must not leak
			gridWindowStart = now;
			gridSamples = 0;
			gridMisses = 0;

		}


		void RecordMiss (bool miss) {

			missRing[missIndex] = miss ? 1 : 0;
			missIndex = (missIndex + 1) & 31;

			int missSum = 0;

			for (int i = 0; i < 32; i++) missSum += missRing[i];

			if (missSum >= 2 && eps < EPS_MAX) {

				eps += 250000.0;

			}

			if (missSum == 0) {

				if (++noMissRun > 240 && eps > EPS_MIN) {

					eps -= 100000.0;
					noMissRun = 0;

				}

			} else {

				noMissRun = 0;

			}

			// the miss verdict doubles as the gear sample (deduped per frame)
			UpdateGridN (miss);

		}


		// two passes re-arm the edge grid; probes are the only exit from free-run
		// (a body-threshold exit would flip-flop the mode every frame)
		void FinishProbe (bool caught, Uint64 missBy) {

			if (!probeFrame) return;

			probeFrame = false;

			Uint64 period = EffectivePeriod ();

			// over-budget body never re-arms: the hair-miss exemption would flip-flop sync/free-run
			if (!caught && period
				&& (missBy > period / 10 || filteredBody > (double) period * gridN)) {

				probePasses = 0;

				return;

			}

			probePasses++;

			if (probePasses >= 2) {

				tearActive = false;
				probePasses = 0;

				// re-anchor the grid: a stale lastPresented would deep-miss and relapse into free-run
				lastPresented = 0;

				// the stale streak must not re-release on the next miss
				deepMissStreak = 0;

				// the gear window is stale from the free-run episode -- fresh sample
				gridWindowStart = SDL_GetTicksNS ();
				gridSamples = 0;
				gridMisses = 0;

				char pBuf[160];

				snprintf (pBuf, sizeof (pBuf),
					"[FramePacer] probe re-entry: body %.2f ms fits the edge budget again -- resuming scanline sync\n",
					filteredBody / 1e6);
				GateLogRaw (pBuf);

			}

		}


		// First boundary after `horizon`, computed in closed form
		Uint64 NextBoundaryAfter (Uint64 horizon, Uint64 period) {

			Uint64 vb = vbLast.load ();

			if (!vb) return 0;

			if (vb > horizon) return vb;

			Uint64 steps = (horizon - vb) / period + 1;

			return vb + steps * period;

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

			snprintf (buf, sizeof (buf),
				"[FramePacer] %s | gate=%d anchor=%s period=%.3f ms scanline=%s\n",
				event,
				gateActive ? 1 : 0,
				fresh ? "live" : "stale",
				EffectivePeriod () / 1e6,
				!scanlineDisabled ? "on" : "off");

			GateLogRaw (buf);

		}


		// log on state change or every 10 s; swaps delta = true presentation rate
		void LogStateHeartbeat () {

			Uint64 now = SDL_GetTicksNS ();

			Uint64 alive = vbAlive.load ();
			bool fresh = alive && now - alive <= ANCHOR_STALE_NS;

			Uint64 state = (gateActive ? 1ULL : 0ULL)
				| (fresh ? 2ULL : 0ULL)
				| (!scanlineDisabled ? 4ULL : 0ULL)
				| (fullscreenMode ? 8ULL : 0ULL);

			if (state == lastGateHash && now - lastGateLogTime < 10000000000ULL) return;

			if (state != lastGateHash && lastGateLogTime && now - lastGateLogTime < 1000000000ULL) return;

			lastGateHash = state;
			lastGateLogTime = now;

			char buf[192];

			snprintf (buf, sizeof (buf),
				"[FramePacer] state | gate=%d fs=%d gridN=%d anchor=%s period=%.3f ms scanline=%s swaps=%u poll=%u/%u body=%.2f ms\n",
				gateActive ? 1 : 0,
				fullscreenMode ? 1 : 0,
				gridN,
				fresh ? "live" : "stale",
				EffectivePeriod () / 1e6,
				!scanlineDisabled ? "on" : "off",
				swapCount, gateHits, gateMisses,
				filteredBody / 1e6);

			swapCount = 0;
			gateHits = 0;
			gateMisses = 0;
			GateLogRaw (buf);

		}


		// fullscreen scanline gate: present exactly on the edge (tear in the
		// blanking zone); false on failure/timeout, caller presents free
		bool WaitVBlankEdge (Uint64 nb, Uint64 period) {

			Uint64 t0 = SDL_GetTicksNS ();

			bool result = WaitVBlankEdgeInner (nb, period);

			// feeding the wait back into rEst would inflate the lead that schedules it
			swapWaitNs += SDL_GetTicksNS () - t0;

			if (result) gateHits++; else gateMisses++;

			return result;

		}

		bool WaitVBlankEdgeInner (Uint64 nb, Uint64 period) {

			if (scanlineDisabled) return false;

			// wait for the anchor to publish the NEXT edge (vbLast jump); not
			// GetScanLine -- it disagrees with the vblank event on multi-screen setups

			// far from the edge: short capped sleeps
			while (true) {

				Uint64 now = SDL_GetTicksNS ();
				Uint64 alive = vbAlive.load ();

				if (!alive || now - alive > ANCHOR_STALE_NS) return false;

				long long pre = (long long) nb - (long long) now;

				if (pre <= (long long) GATE_SLEEP_EDGE_NS) break;

				SDL_DelayNS (pre > 1000000LL ? 1000000ULL : (Uint64) pre);

			}

			// baseline at poll-window entry: with gridN > 1 the sleep loop above
			// crosses skipped edges, a stale baseline fires a period early
			Uint64 v0 = vbLast.load ();

			Uint64 giveUp = SDL_GetTicksNS () + GATE_SLEEP_EDGE_NS + 2000000ULL;

			while (SDL_GetTicksNS () < giveUp) {

				if (vbLast.load () != v0) {

					if (!gateFirstHit) {

						gateFirstHit = true;
						LogState ("scanline edge hit");

					}

					return true;

				}

			}

			return false;

		}


		bool WaitFrameStart (Uint64 target) {

			LogStateHeartbeat ();

			// one serial per frame: the gear window counts a frame exactly once
			frameSerial++;

			// cap semantics changed: a stale Bresenham goal boundary must not leak
			// into other paths' miss verdicts -- drop it (re-anchored on re-entry)
			if (target != lastTarget) capGoalBoundary = 0;

			// the cap rides along for ShouldSwap's submission grid
			lastTarget = target;

			// submission strategy follows the present path (edge gate vs DWM deadlines)
			fullscreenMode = sdlWindow && (SDL_GetWindowFlags (sdlWindow) & SDL_WINDOW_FULLSCREEN) != 0;

			// present-path switch: reset the other path's grid state
			if (fullscreenMode != prevFullscreenMode) {

				prevFullscreenMode = fullscreenMode;
				lastPresented = 0;
				tearActive = false;
				gridN = 1;
				gridMisses = 0;
				gridSamples = 0;
				gridWindowStart = 0;

			}

			// display checks run even when pacing is off (3 s poll, first bind retries every frame)
			if (!lastMonitor || rebindHint.load () || SDL_GetTicksNS () - lastCheckNs >= 3000000000ULL) {

				lastCheckNs = SDL_GetTicksNS ();
				CheckDisplay ();

			}

			if (!sdlWindow || !openAdapter || !waitVBlank) return false;

			Uint64 now = SDL_GetTicksNS ();

			Uint64 alive = vbAlive.load ();

			if (!alive || now - alive > ANCHOR_STALE_NS) {

				gateActive = false;
				return false;

			}

			Uint64 period = EffectivePeriod ();

			if (!period) {

				gateActive = false;
				return false;

			}

			// low cap (target > 1.03P): Bresenham boundary scheduling takes over
			// (was: return false into the pure time-based limiter). goal boundaries
			// live on the real edge grid -- a mix of floor/ceil(target/period)·P
			// steps -- and the carry keeps the mean rate exactly at the cap
			if (!fullscreenMode && target != 0 && target > period + period / 32) {

				double tOverP = (double) target / period;

				int n = (int) std::floor (tOverP + capCarry);

				capCarry = tOverP + capCarry - n;

				if (n < 1) n = 1;

				if (!capGoalBoundary || capGoalBoundary + (period << 1) < now) {

					// first frame / fallen too far behind (miss run or mode switch):
					// re-anchor on the next usable boundary, carry is not fed back
					capGoalBoundary = NextBoundaryAfter (now + (Uint64) eps + (Uint64) rEst, period);

				} else {

					capGoalBoundary += period * n;

				}

				double r = rEst;

				if (r < period * 0.05) r = period * 0.05;
				if (r > period * 0.75) r = period * 0.75;

				long long submitStart = (long long) capGoalBoundary - (long long) eps - (long long) r;

				if (submitStart > (long long) now) {

					Uint64 deadline = (Uint64) submitStart;

					// Never wait more than one period (bad anchor data would stall here)
					if (deadline > now + period) {

						capGoalBoundary = 0;
						gateActive = false;
						frameStartT0 = now;
						return true;

					}

					if (deadline > now + SPIN_MARGIN_NS) {

						SDL_DelayNS (deadline - now - SPIN_MARGIN_NS);

					}

					while (SDL_GetTicksNS () < deadline) {



					}

				}

				// not a gate frame: no ShouldSwap grid accounting, submissions stay
				// per-frame via ContextFlip's SwapWindow; paced=true skips the
				// time-based limiter (the cap is the Bresenham mean)
				gateActive = false;
				frameStartT0 = SDL_GetTicksNS ();
				return true;

			}

			// Never pace on top of vsync
			int swapInterval = 0;

			if (SDL_GL_GetSwapInterval (&swapInterval)) {

				if (swapInterval != 0) {

					gateActive = false;
					return false;

				}

			}

			if (fullscreenMode || target == 0 || target < period - period / 32) {

				// gated mode: render at its own pace, submissions gated in ShouldSwap
				gateActive = true;

				// fullscreen: lock the frame head to the hardware edge clock (unlimited
				// / cap-at-refresh only). over-budget bodies release into free-run;
				// LIME_NO_FULLSCREEN_TEAR pins the grid on. tearActive is set by
				// ShouldSwap on a deep miss, cleared only by probe re-entry

				// one shared verdict for all consumers
				freeRun = fullscreenTear && tearActive;

				// probe every 250 ms while free-running; consecutive passes re-arm back-to-back
				if (freeRun && !probeFrame
					&& (probePasses > 0 || now - lastProbeNs >= 250000000ULL)) {

					probeFrame = true;
					lastProbeNs = now;

				}

				if (!freeRun) overloadLogged = false;

				// fullscreen any target, or windowed with a cap (target != 0); windowed
				// unlimited (target == 0) keeps free-running. windowed gate only reaches
				// here below 0.97P (low caps are taken by the Bresenham branch above),
				// and freeRun/probe are fullscreen-only verdicts (always false there)
				bool edgeAligned = (fullscreenMode || target != 0) && filteredBody > 0.0
					&& (target == 0 || target <= period + period / 32)
					&& (!freeRun || probeFrame);

				if (target != 0 && lastGateStart && !edgeAligned) {

					Uint64 gateDeadline = lastGateStart + target;

					if (gateDeadline > now) {

						if (gateDeadline > now + SPIN_MARGIN_NS) {

							SDL_DelayNS (gateDeadline - now - SPIN_MARGIN_NS);

						}

						while (SDL_GetTicksNS () < gateDeadline) {



						}

						now = SDL_GetTicksNS ();

					}

				}

				// align the render start so the body ends just before the edge
				if (edgeAligned) {

					Uint64 gridPeriod = period * gridN;

					// lead = whole body (commands + drained GPU) + anchor publication
					// delay; the clamp scales with the grid so an N-period body fits
					// fullscreen keeps the fixed margin (the fence drain covers the rest);
					// windowed deadlines sit at DWM composite points with an a-priori
					// unknowable vblank phase -- the adaptive eps covers that offset
					double lead = filteredBody + (fullscreenMode ? (double) EDGE_MARGIN_NS : eps);

					if (lead > (double) gridPeriod * 0.94) lead = (double) gridPeriod * 0.94;

					// probe measures the worst case: grant the full clamped lead
					if (probeFrame) lead = (double) gridPeriod * 0.94;

					if (lead < (double) period * 0.1) lead = (double) period * 0.1;

					long long startAt = (long long) NextBoundaryAfter (now, gridPeriod) - (long long) lead;

					// probe must reach a verdict: slip to the next edge when this one is inside the lead
					if (probeFrame && startAt <= (long long) now) startAt += (long long) gridPeriod;

					if (startAt > (long long) now && startAt - (long long) now < (long long) (gridPeriod * 2)) {

						Uint64 deadline = (Uint64) startAt;

						if (deadline > now + SPIN_MARGIN_NS) {

							SDL_DelayNS (deadline - now - SPIN_MARGIN_NS);

						}

						while (SDL_GetTicksNS () < deadline) {



						}

						now = SDL_GetTicksNS ();

						// probe verdict measures against the edge this frame aligned to
						if (probeFrame) probeEdge = (Uint64) startAt + (Uint64) lead;

					} else if (startAt <= (long long) now) {

						// late frame: re-anchor, render now, align next head (DXVK semantics)
						lastPresented = 0;

						// a probe with failed alignment has no verdict -- drop it
						if (probeFrame) {

							probeFrame = false;
							probePasses = 0;
							probeEdge = 0;

						}

					}

				}

				lastGateStart = now;
				frameStartT0 = now;
				return true;

			}

			gateActive = false;

			if (nextBoundary == 0) {

				frameStartT0 = now;
				return true;

			}

			double r = rEst;

			if (r < period * 0.05) r = period * 0.05;
			if (r > period * 0.75) r = period * 0.75;

			long long submitStart = (long long) nextBoundary - (long long) eps - (long long) r;

			if (submitStart > (long long) now) {

				Uint64 deadline = (Uint64) submitStart;

				// Never wait more than one period (bad anchor data would stall here)
				if (deadline > now + period) {

					nextBoundary = 0;
					frameStartT0 = now;
					return true;

				}

				if (deadline > now + SPIN_MARGIN_NS) {

					SDL_DelayNS (deadline - now - SPIN_MARGIN_NS);

				}

				while (SDL_GetTicksNS () < deadline) {



				}

			}

			frameStartT0 = SDL_GetTicksNS ();
			return true;

		}


		bool ShouldSwap (Uint64 now) {

			// Never gate on top of vsync; the driver already blocks on the edge
			int swapInterval = 0;

			if (SDL_GL_GetSwapInterval (&swapInterval) && swapInterval != 0) return true;

			Uint64 period = EffectivePeriod ();

			if (!period) return true;

			Uint64 nb = NextBoundaryAfter (now, period);

			if (!nb) return true;

			if (fullscreenMode) {

				// free-run: every frame submits at body rate; probes skip this so
				// their verdict can re-arm the grid (lastPresented stays frozen)
				if (freeRun && !probeFrame) {

					swapCount++;
					return true;

				}

				// probe verdict against probeEdge (lastPresented froze with free-run)
				if (probeFrame) {

					if (probeEdge) {

						bool probeCaught = now < probeEdge;

						FinishProbe (probeCaught, probeCaught ? (Uint64) 0 : now - probeEdge);

					} else {

						// aligned edge lost (should not happen): fail conservatively
						FinishProbe (false, 1ULL << 40);

					}

				}

				// whole-divider edge grid: one presentation per grid point, even intervals
				Uint64 gridPeriod = period * gridN;

				Uint64 target;

				// every edge-grid exit routes through FinishProbe
				bool probeCaught = true;

				Uint64 probeMissBy = 0;

				if (!lastPresented) {

					// re-anchor on the first reachable boundary
					target = NextBoundaryAfter (now, gridPeriod);

					RecordMiss (false);

				} else {

					target = lastPresented + gridPeriod;

					if (target <= now) {

						RecordMiss (true);

						probeCaught = false;
						probeMissBy = now - target;

						// every miss presents immediately: hair-miss flips in the blanking
						// zone, deep miss tears but keeps the rate (waiting halved the cadence)
						FinishProbe (probeCaught, probeMissBy);

						lastPresented = target;
						swapCount++;

						// a run of deep misses releases into free-run; isolated misses stay
						if (probeMissBy > period / 10) {

							deepMissStreak++;

							if (deepMissStreak >= 4 && !tearActive) {

								tearActive = true;

								if (fullscreenTear && !overloadLogged) {

									overloadLogged = true;

									char oBuf[192];

									snprintf (oBuf, sizeof (oBuf),
										"[FramePacer] fullscreen overload: body misses the edge by %.2f ms -- free-running at the body rate (no scanline sync, LIME_NO_FULLSCREEN_TEAR reverts to the edge grid)\n",
										probeMissBy / 1e6);
									GateLogRaw (oBuf);

								}

							}

						} else {

							deepMissStreak = 0;

						}

						return true;

					} else {

						// Normal frames count too: the gear window needs them
						RecordMiss (false);

						deepMissStreak = 0;

					}

				}

				FinishProbe (probeCaught, probeMissBy);

				lastPresented = target;

				// missed edge: catch the NEXT edge, never present at a free-running phase
				if (!WaitVBlankEdge (target, period)) {

					WaitVBlankEdge (NextBoundaryAfter (SDL_GetTicksNS () + period / 2, period), period);

				}

				swapCount++;
				return true;

			}

			// windowed: DWM latches the newest frame at each composite point, so EVERY
			// rendered frame submits (flips coalesce). the old submission gate starved
			// the channel into a frozen picture
			gridK = gridN;

			Uint64 step = period * gridK;

			// grid account feeds the gear window; reset on a mode change
			Uint64 target;

			bool overshot = false;

			if (!lastPresented) {

				target = nb + step - period;

			} else {

				target = lastPresented + step;

				if (target <= now) {

					// grid points passed without a submission: re-target forward in
					// whole steps; this is the evidence UpdateGridN climbs the divider with
					target = lastPresented + step * ((now - lastPresented) / step + 1);
					overshot = true;

				} else if (lastPresented > now) {

					// account ahead of the clock hides overshoots and locks the gear
					// out -- re-anchor on the real boundary grid
					target = nb + step - period;

				}

			}

			lastPresented = target;

			// gear sample only; eps stays fed by OnFrameEnd's late-body verdict
			UpdateGridN (overshot);
			swapCount++;
			return true;

		}


		// fullscreen gated modes always drain (undrained swap backpressure bursts the body); free-run is GPU-bound anyway; windowed undrained (DWM latches on its own)
		bool ShouldDrainGpu () {

			return fullscreenMode;

		}


		void OnFrameEnd (Uint64 frameEnd) {

			Uint64 period = EffectivePeriod ();

			if (!period) {

				nextBoundary = 0;
				return;

			}

			Uint64 rInst = frameEnd - frameStartT0;

			// rEst must estimate the render body only -- counting the wait back in runs away
			if (rInst > swapWaitNs) rInst -= swapWaitNs; else rInst = 0;

			Uint64 waitUsed = swapWaitNs;
			swapWaitNs = 0;

			// log raw slow-frame evidence; body vs wait tells stalls from pacing holds
			if (rInst > period + period / 3) {

				slowBurst++;

				// burst raw, then throttle -- periodic GC stalls would flood the log
				if (slowBurst <= 4 || frameEnd - lastSlowLogNs > 2000000000ULL) {

					lastSlowLogNs = frameEnd;

					char sBuf[192];

					snprintf (sBuf, sizeof (sBuf),
						"[FramePacer] slow frame: body=%.2f ms wait=%.2f ms fs=%d gate=%d rEst=%.2f ms\n",
						rInst / 1e6, waitUsed / 1e6,
						fullscreenMode ? 1 : 0, gateActive ? 1 : 0, rEst / 1e6);
					GateLogRaw (sBuf);

				}

			} else {

				slowBurst = 0;

			}

			// keep only a clock-fault guard; the median filter absorbs spikes on its own
			if (rInst > 1000000000ULL) rInst = 1000000000ULL;

			bodyRing[bodyIndex] = rInst;
			bodyIndex = (bodyIndex + 1) % 5;

			// median of the 5-slot ring absorbs spikes
			Uint64 sorted[5];

			for (int i = 0; i < 5; i++) sorted[i] = bodyRing[i];

			for (int i = 1; i < 5; i++) {

				Uint64 v = sorted[i];

				int j = i - 1;

				while (j >= 0 && sorted[j] > v) {

					sorted[j + 1] = sorted[j];
					j--;

				}

				sorted[j + 1] = v;

			}

			// ring starts zeroed: fall back to the raw body until the median is real
			filteredBody = sorted[2] > 0 ? (double) sorted[2] : (double) rInst;

			if (rEst == 0.0) {

				rEst = filteredBody;

			} else {

				// EMA over the median: a spike can no longer inflate it
				rEst = rEst * 7.0 / 8.0 + filteredBody / 8.0;

			}

			if (!fullscreenMode) {

				// windowed tunes eps from late frames; fullscreen pins the edge.
				// low-cap Bresenham: the goal boundary (not nextBoundary, which
				// follows the edge) is this frame's deadline
				RecordMiss (capGoalBoundary != 0
					? frameEnd > capGoalBoundary
					: (nextBoundary != 0 && frameEnd > nextBoundary));

			}

			Uint64 lead = (Uint64) eps + (Uint64) rEst + 1000000ULL;

			nextBoundary = NextBoundaryAfter (frameEnd + lead, period);

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


	bool FramePacer::ShouldDrainGpu () {

		if (!impl) return false;

		return ((FramePacerImpl*) impl)->ShouldDrainGpu ();

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


	bool FramePacer::ShouldDrainGpu () {

		return false;

	}


	void FramePacer::OnFrameEnd (Uint64 frameEnd) {


	}


}


#endif // HX_WINDOWS
