// ============================================================================
//  NQ Edge Suite — directional-intelligence cockpit for NQ/MNQ (Sierra Chart ACSIL)
//
//  One self-contained DLL. Studies exported (add in this order):
//    1. NQ Edge: Auction/Structure Engine     scsf_NQEdge_Auction
//    2. NQ Edge: VWAP Engine                  scsf_NQEdge_VWAP
//    3. NQ Edge: Order Flow Engine            scsf_NQEdge_OrderFlow
//    4. NQ Edge: Regime + MTF Bias            scsf_NQEdge_Regime
//    5. NQ Edge: Intermarket Engine           scsf_NQEdge_Intermarket
//    6. NQ Edge: Directional Conviction Score scsf_NQEdge_DCS
//    7. NQ Edge: Signal Validation            scsf_NQEdge_Validation
//    8. NQ Edge: Feature Logger               scsf_NQEdge_FeatureLogger
//    9. NQ Edge: HUD + Bar Painter            scsf_NQEdge_HUD
//
//  Architecture (docs/ARCHITECTURE.md): every study shares one per-chart ChartState held in a
//  DLL-global registry. Engines are lazy and idempotent (EnsureBase/EnsureAuction/...), so the
//  order in which Sierra calls the studies does not matter. All decision-relevant values are
//  final only on closed bars; the forming bar is provisional and never enters statistics.
//
//  Section index (search for "// ==== "):
//    ==== 0  Includes, DLL name, small helpers
//    ==== 1  Params (inputs copied from each study)
//    ==== 2  Engine states + ChartState + registry
//    ==== 3  Stamp / reset / params versioning
//    ==== 4  Base pass (ATR, sessions)
//    ==== 5  Drawing helpers
//    ==== 6  Auction / Structure engine
//    ==== 7  VWAP engine
//    ==== 8  Order Flow engine
//    ==== 9  Regime + MTF engine
//    ==== 10 Intermarket engine
//    ==== 11 Weights + DCS composite + setups
//    ==== 12 Validation
//    ==== 13 Feature logger
//    ==== 14 HUD snapshot + GDI
//    ==== 15 Study functions (scsf_*)
// ============================================================================

// ==== 0  Includes, DLL name, small helpers ==================================
#include "sierrachart.h"

#include <vector>
#include <map>
#include <set>
#include <climits>
#include <algorithm>
#include <mutex>
#include <string>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cfloat>
#include <sys/stat.h>

SCDLLName("NQ Edge Suite")

namespace nqe
{
	// windows.h defines min/max macros (Sierra does not define NOMINMAX), so std::min/max are
	// unusable here. Local helpers instead.
	template<class T> inline T Min(T a, T b) { return a < b ? a : b; }
	template<class T> inline T Max(T a, T b) { return a > b ? a : b; }
	template<class T> inline T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }
	inline float Clamp1(double v) { return static_cast<float>(v < -1.0 ? -1.0 : (v > 1.0 ? 1.0 : v)); }
	inline float Sign(double v) { return v > 0 ? 1.0f : (v < 0 ? -1.0f : 0.0f); }
	inline bool IsNan(float v) { return v != v; }
	inline bool IsNan(double v) { return v != v; }
	static const float kNaN = std::numeric_limits<float>::quiet_NaN();
	inline float SafeDiv(double a, double b, float dflt = 0.0f) { return b != 0.0 ? static_cast<float>(a / b) : dflt; }

	// Sierra's own GetBarHasClosedStatus: every bar except the last one has closed.
	inline bool BarClosed(SCStudyInterfaceRef sc, int i) { return sc.GetBarHasClosedStatus(i) == BHCS_BAR_HAS_CLOSED; }

	// Storage time unit value that means "1 tick" (see docs/DECISIONS.md, unverified).
	static const unsigned int kStorageUnitTick = 0;

	enum Regime { RG_NONE = 0, RG_TREND_UP = 1, RG_TREND_DOWN = 2, RG_BALANCE = 3, RG_CHOP = 4 };
	enum OpenType
	{
		OT_NONE = 0, OT_DRIVE_UP, OT_DRIVE_DOWN, OT_TEST_DRIVE_UP, OT_TEST_DRIVE_DOWN,
		OT_REJECT_REVERSE_UP, OT_REJECT_REVERSE_DOWN, OT_AUCTION
	};
	enum SetupType
	{
		SETUP_NONE = 0, SETUP_TREND_PULLBACK = 1, SETUP_VALUE_EDGE = 2, SETUP_FAILED_BREAKOUT = 3,
		SETUP_BREAK_ACCEPT = 4, SETUP_DIVERGENCE = 5, SETUP_COUNT = 6
	};
	static const char* kSetupNames[SETUP_COUNT] =
	{ "None", "Trend Pullback", "Value Edge", "Failed Breakout", "Break+Accept", "Divergence" };
	static const char* kRegimeNames[5] = { "--", "TREND UP", "TREND DOWN", "BALANCE", "VOLATILE CHOP" };
	static const char* kOpenTypeNames[8] =
	{ "--", "Open-Drive Up", "Open-Drive Down", "Open-Test-Drive Up", "Open-Test-Drive Down",
	  "Open-Reject-Reverse Up", "Open-Reject-Reverse Down", "Open-Auction" };

	// ==== 1  Params =============================================================
	// Plain structs so a byte compare can detect input changes. Always construct with {}.

	struct BaseParams
	{
		int rthStartSec = 9 * 3600 + 30 * 60;
		int rthEndSec = 16 * 3600;
		int atrLength = 14;
	};

	struct AuctionParams
	{
		int ibMinutes = 60;
		int openTypeMinutes = 30;
		float valueAreaPct = 70.0f;
		int profileTicksPerLevel = 1;
		int nakedPocsTracked = 10;
		int tpoMinutes = 30;
		int swingStrength = 5;
		float swingMinAtr = 0.5f;
		int equalTolTicks = 2;
		int bosDecayBars = 10;
		float ibExtA = 0.5f, ibExtB = 1.0f, ibExtC = 2.0f;
	};

	struct VwapParams
	{
		int anchor = 0;            // 0 = RTH open, 1 = trading day start
		float band1 = 1.0f, band2 = 2.0f, band3 = 3.0f;
		int slopeBars = 10;
		int acceptCloses = 3;
	};

	struct FlowParams
	{
		int cvdReset = 0;          // 0 = RTH open, 1 = trading day, 2 = never
		int cvdSlopeBars = 10;
		int volZLength = 50;
		float absorbVolZ = 2.0f;
		float absorbMaxRangeAtr = 0.6f;
		float absorbZoneFrac = 0.25f;
		int exhaustRunBars = 3;
		float exhaustExtremePct = 15.0f;
		float imbRatioPct = 300.0f;
		int imbMinVolume = 10;
		int imbStackLevels = 3;
		float largePercentile = 99.0f;
		int largeMinSize = 20;
		int largeLookback = 2000;
		int maxBubbles = 100;
		int trapLookback = 20;
		int trapReversalBars = 3;
		float trapMinDeltaPct = 20.0f;
		float divMinAtr = 0.5f;
		int eventDecayBars = 8;
		int maxActiveZones = 30;
	};

	struct RegimeParams
	{
		int erLength = 20;
		float erTrend = 0.35f;
		float erBalance = 0.20f;
		int atrFast = 7;
		int atrSlow = 50;
		float chopExpansion = 1.3f;
		int ibAvgDays = 20;
		int insideValueBars = 30;
		int hysteresisBars = 3;
		int mtfEmaLength = 20;
		int mtfSwing = 3;
	};

	struct InterParams
	{
		int chartYM = 0, chartES = 0, chartRTY = 0, chartTICK = 0;
		int chartMega[6] = { 0, 0, 0, 0, 0, 0 };
		int rsLookback = 20;
		int rsZLength = 100;
		float tickExtreme = 800.0f;
		float tickStrong = 1000.0f;
		int tickLookback = 10;
		int tickEma = 10;
		int megaEma = 20;
	};

	struct DcsParams
	{
		char weightsFile[128] = "NQEdge_weights.txt";
		int hotReloadSec = 5;
		float signalThr = 40.0f;
		float fadeThr = 20.0f;
		int smoothLen = 5;
		float strongThr = 60.0f;
		float weakThr = 25.0f;
		int setupOn[SETUP_COUNT] = { 0, 1, 1, 1, 1, 1 };
		int requireMtf = 1;
		float levelTolAtr = 0.3f;
		float stopBufferAtr = 0.5f;
		float minRR = 1.0f;
		float minTargetAtr = 0.5f;
		int maxSignalsDrawn = 30;
		int targetLineBars = 20;
		int alertsOn = 1;
		int alertSound = 1;
	};

	struct ValParams
	{
		int slippageTicks = 1;
		int maxBars = 120;
		int stopFirst = 1;
		int minSample = 30;
	};

	struct LogParams
	{
		int enabled = 1;
		char prefix[64] = "NQEdge_features";
		int rewriteOnRecalc = 1;
		int rthOnly = 0;
	};

	struct Params
	{
		BaseParams base; AuctionParams auction; VwapParams vwap; FlowParams flow;
		RegimeParams regime; InterParams inter; DcsParams dcs; ValParams val; LogParams log;
	};

	template<class T> inline bool SameBytes(const T& a, const T& b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }

	// ==== 2  Engine states + ChartState + registry =============================

	enum EngineId { E_BASE = 0, E_AUCTION, E_VWAP, E_FLOW, E_REGIME, E_INTER, E_DCS, E_VAL, E_LOG, E_COUNT };

	struct EngineCommon
	{
		int computedThrough = -1;   // last closed bar with final values
		int generation = 0;         // bumped on every reset (studies use it to rewrite subgraphs)
		int lastArraySize = 0;
		int dirtyFrom = INT_MAX;    // lowest historical index rewritten since the owning study last mirrored
		// fingerprint of the forming bar when this engine last ran: lets repeated Ensure* calls in one
		// update cycle (several studies share the engine) skip re-evaluating an unchanged last bar
		int fpN = 0; double fpTime = 0; float fpVol = -1, fpClose = 0, fpHigh = 0, fpLow = 0;
		void Reset() { computedThrough = -1; ++generation; lastArraySize = 0; dirtyFrom = INT_MAX; fpN = 0; fpVol = -1; }
		bool UpToDate(SCStudyInterfaceRef sc) const
		{
			const int n = sc.ArraySize; if (n <= 0 || computedThrough < n - 2 || lastArraySize != n || fpN != n) return false;
			const int i = n - 1;
			return fpTime == sc.BaseDateTimeIn[i].GetAsDouble() && fpVol == sc.Volume[i] && fpClose == sc.Close[i] && fpHigh == sc.High[i] && fpLow == sc.Low[i];
		}
		void Stamp(SCStudyInterfaceRef sc)
		{
			const int n = sc.ArraySize; computedThrough = n - 2; lastArraySize = n; fpN = n;
			if (n > 0) { const int i = n - 1; fpTime = sc.BaseDateTimeIn[i].GetAsDouble(); fpVol = sc.Volume[i]; fpClose = sc.Close[i]; fpHigh = sc.High[i]; fpLow = sc.Low[i]; }
		}
	};

	struct BaseState : EngineCommon
	{
		std::vector<float> atr, tr;
		std::vector<int> tradingDay;      // sc.GetTradingDayDate per bar
		std::vector<unsigned char> isRth;
		std::vector<int> rthSession;      // increasing id, -1 outside RTH
		std::vector<int> secIntoRth;      // seconds since RTH start (RTH bars only)
		std::vector<int> dayStartIdx;     // first bar index of this trading day
		std::vector<int> rthStartIdx;     // first RTH bar index of this trading day (-1 if none yet)
		std::vector<int> lastRthId;       // highest RTH session id seen up to and including bar i
	};

	// Price level published by the auction engine (for HUD distances and setup targets).
	enum LevelKind
	{
		LVL_NONE = 0, LVL_POC, LVL_VAH, LVL_VAL, LVL_PD_POC, LVL_PD_VAH, LVL_PD_VAL, LVL_PDH, LVL_PDL,
		LVL_ONH, LVL_ONL, LVL_IBH, LVL_IBL, LVL_IBEXT, LVL_NAKED_POC, LVL_SWING_H, LVL_SWING_L,
		LVL_LIQ_EQH, LVL_LIQ_EQL, LVL_VWAP, LVL_VWAP_B1U, LVL_VWAP_B1D, LVL_VWAP_B2U, LVL_VWAP_B2D,
		LVL_VWAP_B3U, LVL_VWAP_B3D, LVL_ABSORB, LVL_IMB, LVL_SINGLE_PRINT
	};
	struct Level { float price; int kind; int bornIdx; };

	struct Swing { int idx; float price; int confirmIdx; bool high; };

	struct Zone
	{
		int bornIdx = 0; float top = 0, bottom = 0; int dir = 0;   // dir +1 = support-type (bull), -1 = resistance-type (bear)
		int kind = 0; bool active = true; int lineNumber = 0; int deadIdx = -1;
	};

	struct AuctionState : EngineCommon
	{
		// per-bar outputs
		std::vector<float> poc, vah, val;                 // developing RTH profile
		std::vector<float> pdPoc, pdVah, pdVal, pdh, pdl; // prior day
		std::vector<float> onHigh, onLow;                 // overnight range (dev. during ON, frozen in RTH)
		std::vector<float> ibHigh, ibLow;                 // initial balance (dev. during IB, frozen after)
		std::vector<unsigned char> ibDone;
		std::vector<float> structTrend, bos, vaPos, pocPos, ibPos, valueMig, openTypeDir;
		std::vector<signed char> openType;
		std::vector<signed char> swingHighMark, swingLowMark; // 1 at the pivot bar (set at confirmation)
		std::vector<signed char> bosMark, chochMark;          // +1/-1 at the bar that broke
		// swings
		std::vector<Swing> swings;
		int lastSwingHigh = -1, lastSwingLow = -1;           // indices into swings
		int prevSwingHigh = -1, prevSwingLow = -1;
		bool lastHighBroken = false, lastLowBroken = false;
		// RTH profile (committed through computedThrough)
		std::map<int, double> profVol;                       // level -> volume
		double profTotal = 0;
		std::map<int, int> tpoMap;                           // level -> TPO count (completed periods)
		std::set<int> periodLevels;                          // levels touched in the current TPO period
		int lastTpoPeriod = -1;
		float devPoc = 0, devVah = 0, devVal = 0;
		// day / session bookkeeping
		int curDay = 0;
		int finalizedSession = -1, rthOpenSession = -1;
		float onH = -FLT_MAX, onL = FLT_MAX;
		float rthHigh = -FLT_MAX, rthLow = FLT_MAX;
		float ibH = -FLT_MAX, ibL = FLT_MAX; bool ibClosed = false; int ibCloseIdx = -1;
		float prevDayHigh = 0, prevDayLow = 0, prevPoc = 0, prevVah = 0, prevVal = 0;
		float rthOpen = 0; int rthOpenIdx = -1;
		float o30High = -FLT_MAX, o30Low = FLT_MAX; int o30HighIdx = -1, o30LowIdx = -1;
		signed char todayOpenType = OT_NONE; bool openTypeDone = false; float openTypeDirValue = 0;
		std::vector<float> ibRangeHistory;                   // one per completed IB (for regime)
		std::vector<float> nakedPocs;                        // untested prior-session POCs
		std::vector<int> nakedPocBorn;
		std::vector<int> nakedPocLine;
		std::vector<Zone> singlePrints;
		std::vector<Zone> liquidity;                         // equal highs/lows
		std::vector<Level> levels;                           // snapshot of all active levels (rebuilt each update)
		std::vector<int> deadLines;                          // drawings to delete (flushed by the Auction study)
		int lastBosDir = 0, lastBosIdx = -1000;
		int lastChochDir = 0, lastChochIdx = -1000;
	};

	struct VwapState : EngineCommon
	{
		std::vector<float> vwap, sd, b1u, b1d, b2u, b2d, b3u, b3d;
		std::vector<float> avOn, avRth, avSwHi, avSwLo;
		std::vector<float> slope, pos, accept;     // features
		// committed sums (through computedThrough)
		double sPV = 0, sPPV = 0, sV = 0; int sSession = -1; int sStartIdx = -1;
		double oPV = 0, oV = 0; int oDay = 0;
		double rPV = 0, rV = 0; int rSession = -1;
		double hPV = 0, hV = 0; int hAnchor = -1;
		double lPV = 0, lV = 0; int lAnchor = -1;
		int acceptRun = 0;                          // consecutive closes on one side (signed)
	};

	struct Bubble { int idx; float price; double size; int dir; bool live; int lineNumber; };

	struct FlowState : EngineCommon
	{
		std::vector<float> delta, deltaPct, cvd, cvdZ, volZ;
		std::vector<float> fAbsorb, fExhaust, fImb, fTrapped, fCvdDiv, fLarge;   // features
		std::vector<signed char> absorbMark, exhaustMark, imbMark, trapMark, divMark;
		std::vector<double> volPre1, volPre2, dzPre1, dzPre2;                   // prefix sums over closed bars
		std::vector<float> cvdDz;
		double cvdCommitted = 0; int cvdSession = -1; int cvdDay = 0;
		std::vector<Zone> absorbZones, imbZones;
		std::vector<Bubble> bubbles;
		std::vector<double> tradeSizes;             // rolling sample for live percentile
		unsigned int lastTsSequence = 0;
		int liveSampleCount = 0; double liveThreshold = 0;
		std::vector<double> levelAvgSizes;          // rolling sample of VAP avg-trade-size per level
		int ltSampleCount = 0; double ltThreshold = 0;
		int lastProcessedSwing = -1;
		// trapped: pending breakout candidates
		struct Breakout { int idx; int dir; float extreme; };
		std::vector<Breakout> pendingBreakouts;
		// last events (for decayed features)
		int lastAbsIdx = -1000, lastAbsDir = 0, lastExhIdx = -1000, lastExhDir = 0, lastImbIdx = -1000, lastImbDir = 0;
		int lastTrapIdx = -1000, lastTrapDir = 0, lastDivIdx = -1000, lastDivDir = 0; float lastDivPrice = 0;
		struct Marker { int idx; int dir; int kind; float price; int lineNumber; };
		std::vector<Marker> markers;
		std::vector<int> deadLines;
		float lastEventPrice = 0; int lastEventIdx = -1; char lastEventText[64] = "";
	};

	struct RegimeState : EngineCommon
	{
		std::vector<float> er, atrRatio, regimeTrend, mtfBias, trendiness, dirScore;
		std::vector<signed char> regime, mtf1, mtf5, mtf15, mtf60;
		int candidate = RG_NONE; int candidateCount = 0; int current = RG_NONE;
		std::vector<float> atrFast, atrSlow;
		std::vector<double> absDcPre, insidePre;      // prefix sums over closed bars
		struct TfBar { int startIdx = -1; int count = 0; float o = 0, h = 0, l = 0, c = 0; };
		struct Tf
		{
			std::vector<TfBar> bars; TfBar cur; int curKey = INT_MIN; int committedKey = INT_MIN;
			float ema = 0; int emaCount = 0; std::vector<float> emaHist;
			float lastHigh = 0, prevHigh = 0, lastLow = 0, prevLow = 0; bool available = true;
		};
		Tf tf[4];
	};

	struct RefChart
	{
		int chartNumber = 0; bool ok = false; SCString symbol;
		std::vector<double> cumPV, cumV; int cumThrough = -1;   // for ref VWAP
		std::vector<float> ema; int emaThrough = -1;
		int lastRefSize = 0;
	};

	struct InterState : EngineCommon
	{
		std::vector<float> rsYM, rsES, rsRTY, rsIndex, smt, tickCum, tickExt, tickDiv, megaCap, composite;
		std::vector<signed char> smtMark;
		RefChart ym, es, rty, tick, mega[6];
		int available = 0;                           // bitmask: 1 YM, 2 ES, 4 RTY, 8 TICK, 16.. mega
		double tickCumCommitted = 0; int tickSession = -1;
		int lastSwingProcessed = -1;
		std::vector<float> rsRet; // scratch
	};

	// Feature vector published to the composite. Index constants must match kFeatureNames.
	enum FeatureId
	{
		F_VWAP_POS = 0, F_VWAP_SLOPE, F_VWAP_ACCEPT, F_STRUCT_TREND, F_BOS, F_VA_POS, F_POC_POS, F_IB_POS,
		F_VALUE_MIG, F_OPEN_TYPE, F_DELTA, F_CVD_Z, F_CVD_DIV, F_ABSORB, F_EXHAUST, F_IMBALANCE, F_TRAPPED,
		F_LARGE_TRADE, F_REGIME_TREND, F_MTF_BIAS, F_RS_INDEX, F_SMT, F_TICK_CUM, F_TICK_EXT, F_TICK_DIV,
		F_MEGA_CAP, F_COUNT
	};
	static const char* kFeatureNames[F_COUNT] =
	{
		"vwapPos", "vwapSlope", "vwapAccept", "structTrend", "bos", "vaPos", "pocPos", "ibPos",
		"valueMig", "openType", "delta", "cvdZ", "cvdDiv", "absorb", "exhaust", "imbalance", "trapped",
		"largeTrade", "regimeTrend", "mtfBias", "rsIndex", "smt", "tickCum", "tickExt", "tickDiv", "megaCap"
	};
	enum FeatureGroup { G_TREND = 0, G_FLOW, G_REVERSAL, G_LOCATION, G_INTERMARKET, G_CONTEXT, G_COUNT };
	static const char* kGroupNames[G_COUNT] = { "trend", "flow", "reversal", "location", "intermarket", "context" };
	static const int kFeatureGroup[F_COUNT] =
	{
		G_LOCATION, G_TREND, G_TREND, G_TREND, G_TREND, G_LOCATION, G_LOCATION, G_LOCATION,
		G_CONTEXT, G_CONTEXT, G_FLOW, G_FLOW, G_REVERSAL, G_REVERSAL, G_REVERSAL, G_FLOW, G_REVERSAL,
		G_FLOW, G_TREND, G_TREND, G_INTERMARKET, G_REVERSAL, G_INTERMARKET, G_INTERMARKET, G_REVERSAL,
		G_INTERMARKET
	};

	struct Weights
	{
		float w[F_COUNT];
		float gate[5][G_COUNT];     // by regime (index by Regime enum)
		float thrSignal = 40.0f, thrFade = 20.0f;
		int version = 1;
		time_t fileMtime = 0; double lastCheck = 0; bool loadedFromFile = false;
		void SetDefaults()
		{
			static const float d[F_COUNT] =
			{
				0.6f, 0.8f, 0.5f, 0.9f, 0.6f, 0.5f, 0.4f, 0.3f,
				0.4f, 0.4f, 0.6f, 0.8f, 0.7f, 0.8f, 0.5f, 0.7f, 0.8f,
				0.5f, 1.0f, 0.9f, 0.5f, 0.7f, 0.4f, 0.4f, 0.5f, 0.5f
			};
			for (int i = 0; i < F_COUNT; ++i) w[i] = d[i];
			for (int r = 0; r < 5; ++r) for (int g = 0; g < G_COUNT; ++g) gate[r][g] = 1.0f;
			// Trend: favor continuation, damp reversal. Balance: invert location (fade edges), damp trend.
			gate[RG_TREND_UP][G_TREND] = 1.2f;  gate[RG_TREND_UP][G_REVERSAL] = 0.5f;  gate[RG_TREND_UP][G_LOCATION] = 0.5f;
			gate[RG_TREND_DOWN][G_TREND] = 1.2f; gate[RG_TREND_DOWN][G_REVERSAL] = 0.5f; gate[RG_TREND_DOWN][G_LOCATION] = 0.5f;
			gate[RG_BALANCE][G_TREND] = 0.6f; gate[RG_BALANCE][G_REVERSAL] = 1.3f; gate[RG_BALANCE][G_LOCATION] = -0.8f;
			for (int g = 0; g < G_COUNT; ++g) gate[RG_CHOP][g] = 0.5f;
			thrSignal = 40.0f; thrFade = 20.0f;
		}
		Weights() { SetDefaults(); }
	};

	struct Signal
	{
		int idx = 0; int type = SETUP_NONE; int dir = 0; float entry = 0, stop = 0, t1 = 0, t2 = 0;
		float dcs = 0; float rr = 0; char label[96] = "";
		int lineArrow = 0, lineEntry = 0, lineStop = 0, lineT1 = 0, lineT2 = 0, lineText = 0;
		// validation (filled by the validation engine)
		int resolved = 0;          // 0 pending, 1 win (T1), -1 loss, 2 timeout
		int hitT2 = 0; float mfeR = 0, maeR = 0; int barsToRes = 0; float resultR = 0; int resIdx = -1;
	};

	struct DcsState : EngineCommon
	{
		std::vector<float> feat;         // F_COUNT per bar (row-major)
		std::vector<float> dcs, dcsSmooth;
		std::vector<signed char> barState; // -2..+2
		std::vector<signed char> signalType, signalDir;
		Weights weights;
		std::vector<Signal> signals;
		int lastAlertIdx = -1;
		int lastSignalCheckedIdx = -1;
	};

	struct SetupStats { int count = 0, wins = 0, losses = 0, t2 = 0, timeouts = 0; double sumR = 0, sumWinR = 0, sumLossR = 0, sumMfe = 0, sumMae = 0, sumBars = 0; };

	struct ValState : EngineCommon
	{
		SetupStats stats[SETUP_COUNT];
		std::vector<float> cumR;
		int nextSignal = 0;      // first signal index not yet fully resolved
		double totalR = 0;
	};

	struct LogRow
	{
		int idx; double t; float o, h, l, c, v; float feat[F_COUNT]; float dcs; int regime; int setup; int dir;
		float atr; float fwd5, fwd15, fwd30, fwd60, mfe, mae; bool done;
	};

	struct LogState : EngineCommon
	{
		std::vector<LogRow> pending;
		int lastQueuedIdx = -1;
		bool headerWritten = false;
		std::string path;
	};

	struct Warnings
	{
		bool storageNotTick = false, tzNotNY = false, vapOff = false, noDepth = false, noTS = false;
		char interMissing[160] = "";
		char text[512] = "";
	};

	struct HudSnapshot
	{
		int lastClosedIdx = -1;
		float dcs = 0, dcsSmooth = 0, dcsTrend = 0; int barState = 0; int bias = 0;
		int regime = 0; int openType = 0; float valueMig = 0;
		int mtf[4] = { 0, 0, 0, 0 }; bool mtfAvail[4] = { false, false, false, false };
		int ym = 0, tick = 0, mega[6] = { 0, 0, 0, 0, 0, 0 }; int megaCount = 0; char megaNames[6][12] = {};
		bool ymAvail = false, tickAvail = false; int smt = 0;
		float cvdZ = 0; int cvdTrend = 0; char lastEvent[64] = ""; float lastEventPrice = 0;
		float supPrice = 0, resPrice = 0; int supKind = 0, resKind = 0; float atr = 0; float close = 0;
		char stateLine[160] = "";
		int curSetup = 0; SetupStats curStats; bool statsAvail = false;
		char vwapText[64] = "";
		int signalsTotal = 0;
	};

	struct DataStamp { int arraySize = 0; double t0 = 0, tMid = 0, tLast = 0; int midIdx = -1, lastIdx = -1; bool valid = false; };

	struct ChartState
	{
		int chartNumber = 0;
		int refCount = 0;
		DataStamp stamp;
		Params params;
		bool paramsSet[E_COUNT] = { false, false, false, false, false, false, false, false, false };
		bool studyPresent[E_COUNT] = { false, false, false, false, false, false, false, false, false };
		BaseState base; AuctionState auction; VwapState vwap; FlowState flow; RegimeState regime;
		InterState inter; DcsState dcs; ValState val; LogState log;
		Warnings warn; HudSnapshot hud;
		float tickSize = 0.25f;
		int resetCount = 0;

		EngineCommon& Engine(int e)
		{
			switch (e)
			{
			case E_BASE: return base; case E_AUCTION: return auction; case E_VWAP: return vwap;
			case E_FLOW: return flow; case E_REGIME: return regime; case E_INTER: return inter;
			case E_DCS: return dcs; case E_VAL: return val; default: return log;
			}
		}
	};

	static std::recursive_mutex g_mutex;
	static std::map<int, ChartState*> g_charts;
	static int g_instanceId = 0;   // random per DLL load, lets study instances detect a reload

	inline int InstanceId()
	{
		if (g_instanceId == 0) g_instanceId = static_cast<int>(GetTickCount64() & 0x7fffffff) | 1;
		return g_instanceId;
	}

	static const int kPersistAttached = 990001;

	// Returns the chart state, creating it on first use. Each study instance attaches once.
	ChartState& Acquire(SCStudyInterfaceRef sc)
	{
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		ChartState*& S = g_charts[sc.ChartNumber];
		if (S == nullptr) { S = new ChartState(); S->chartNumber = sc.ChartNumber; }
		int& attached = sc.GetPersistentInt(kPersistAttached);
		if (attached != InstanceId()) { attached = InstanceId(); ++S->refCount; }
		S->tickSize = sc.TickSize > 0 ? sc.TickSize : 0.25f;
		return *S;
	}

	void Release(SCStudyInterfaceRef sc, int engine)
	{
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		std::map<int, ChartState*>::iterator it = g_charts.find(sc.ChartNumber);
		if (it == g_charts.end()) return;
		ChartState* S = it->second;
		int& attached = sc.GetPersistentInt(kPersistAttached);
		if (attached == InstanceId()) { attached = 0; --S->refCount; }
		if (engine >= 0 && engine < E_COUNT) S->studyPresent[engine] = false;
		if (S->refCount <= 0) { delete S; g_charts.erase(it); }
	}

	ChartState* Peek(int chartNumber)
	{
		std::map<int, ChartState*>::iterator it = g_charts.find(chartNumber);
		return it == g_charts.end() ? nullptr : it->second;
	}

	// ==== 3  Stamp / reset / params versioning ==================================

	void ResetFrom(ChartState& S, int engine)
	{
		for (int e = engine; e < E_COUNT; ++e) S.Engine(e).Reset();
		if (engine <= E_AUCTION)
		{
			S.auction.swings.clear(); S.auction.lastSwingHigh = S.auction.lastSwingLow = -1;
			S.auction.prevSwingHigh = S.auction.prevSwingLow = -1; S.auction.lastHighBroken = S.auction.lastLowBroken = false;
			S.auction.profVol.clear(); S.auction.profTotal = 0; S.auction.tpoMap.clear(); S.auction.periodLevels.clear(); S.auction.lastTpoPeriod = -1;
			S.auction.devPoc = S.auction.devVah = S.auction.devVal = 0;
			S.auction.curDay = 0; S.auction.finalizedSession = -1; S.auction.rthOpenSession = -1;
			S.auction.onH = -FLT_MAX; S.auction.onL = FLT_MAX; S.auction.rthHigh = -FLT_MAX; S.auction.rthLow = FLT_MAX;
			S.auction.ibH = -FLT_MAX; S.auction.ibL = FLT_MAX; S.auction.ibClosed = false; S.auction.ibCloseIdx = -1;
			S.auction.prevDayHigh = S.auction.prevDayLow = S.auction.prevPoc = S.auction.prevVah = S.auction.prevVal = 0;
			S.auction.rthOpen = 0; S.auction.rthOpenIdx = -1; S.auction.o30High = -FLT_MAX; S.auction.o30Low = FLT_MAX; S.auction.o30HighIdx = S.auction.o30LowIdx = -1;
			S.auction.todayOpenType = OT_NONE; S.auction.openTypeDone = false; S.auction.openTypeDirValue = 0;
			S.auction.ibRangeHistory.clear();
			for (size_t k = 0; k < S.auction.nakedPocLine.size(); ++k) if (S.auction.nakedPocLine[k]) S.auction.deadLines.push_back(S.auction.nakedPocLine[k]);
			for (size_t k = 0; k < S.auction.singlePrints.size(); ++k) if (S.auction.singlePrints[k].lineNumber) S.auction.deadLines.push_back(S.auction.singlePrints[k].lineNumber);
			for (size_t k = 0; k < S.auction.liquidity.size(); ++k) if (S.auction.liquidity[k].lineNumber) S.auction.deadLines.push_back(S.auction.liquidity[k].lineNumber);
			S.auction.nakedPocs.clear(); S.auction.nakedPocBorn.clear(); S.auction.nakedPocLine.clear();
			S.auction.singlePrints.clear(); S.auction.liquidity.clear(); S.auction.levels.clear();
			S.auction.lastBosDir = 0; S.auction.lastBosIdx = -1000; S.auction.lastChochDir = 0; S.auction.lastChochIdx = -1000;
		}
		if (engine <= E_VWAP)
		{
			S.vwap.sPV = S.vwap.sPPV = S.vwap.sV = 0; S.vwap.sSession = -1; S.vwap.sStartIdx = -1;
			S.vwap.oPV = S.vwap.oV = 0; S.vwap.oDay = 0; S.vwap.rPV = S.vwap.rV = 0; S.vwap.rSession = -1;
			S.vwap.hPV = S.vwap.hV = 0; S.vwap.hAnchor = -1; S.vwap.lPV = S.vwap.lV = 0; S.vwap.lAnchor = -1; S.vwap.acceptRun = 0;
		}
		if (engine <= E_FLOW)
		{
			S.flow.cvdCommitted = 0; S.flow.cvdSession = -1; S.flow.cvdDay = 0;
			for (size_t k = 0; k < S.flow.absorbZones.size(); ++k) if (S.flow.absorbZones[k].lineNumber) S.flow.deadLines.push_back(S.flow.absorbZones[k].lineNumber);
			for (size_t k = 0; k < S.flow.imbZones.size(); ++k) if (S.flow.imbZones[k].lineNumber) S.flow.deadLines.push_back(S.flow.imbZones[k].lineNumber);
			for (size_t k = 0; k < S.flow.bubbles.size(); ++k) if (S.flow.bubbles[k].lineNumber) S.flow.deadLines.push_back(S.flow.bubbles[k].lineNumber);
			for (size_t k = 0; k < S.flow.markers.size(); ++k) if (S.flow.markers[k].lineNumber) S.flow.deadLines.push_back(S.flow.markers[k].lineNumber);
			S.flow.absorbZones.clear(); S.flow.imbZones.clear(); S.flow.bubbles.clear(); S.flow.markers.clear();
			S.flow.tradeSizes.clear(); S.flow.lastTsSequence = 0; S.flow.liveSampleCount = 0; S.flow.liveThreshold = 0;
			S.flow.levelAvgSizes.clear(); S.flow.ltSampleCount = 0; S.flow.ltThreshold = 0; S.flow.lastProcessedSwing = -1; S.flow.pendingBreakouts.clear();
			S.flow.lastAbsIdx = S.flow.lastExhIdx = S.flow.lastImbIdx = S.flow.lastTrapIdx = S.flow.lastDivIdx = -1000;
			S.flow.lastAbsDir = S.flow.lastExhDir = S.flow.lastImbDir = S.flow.lastTrapDir = S.flow.lastDivDir = 0; S.flow.lastDivPrice = 0;
			S.flow.lastEventIdx = -1; S.flow.lastEventText[0] = 0;
		}
		if (engine <= E_REGIME) { S.regime.candidate = RG_NONE; S.regime.candidateCount = 0; S.regime.current = RG_NONE; for (int t = 0; t < 4; ++t) S.regime.tf[t] = RegimeState::Tf(); }
		if (engine <= E_INTER)
		{
			S.inter.tickCumCommitted = 0; S.inter.tickSession = -1; S.inter.lastSwingProcessed = -1;
			RefChart* refs[10] = { &S.inter.ym, &S.inter.es, &S.inter.rty, &S.inter.tick, &S.inter.mega[0], &S.inter.mega[1], &S.inter.mega[2], &S.inter.mega[3], &S.inter.mega[4], &S.inter.mega[5] };
			for (int k = 0; k < 10; ++k) { refs[k]->cumThrough = -1; refs[k]->emaThrough = -1; refs[k]->cumPV.clear(); refs[k]->cumV.clear(); refs[k]->ema.clear(); refs[k]->lastRefSize = 0; }
		}
		if (engine <= E_DCS) { S.dcs.signals.clear(); S.dcs.lastAlertIdx = -1; S.dcs.lastSignalCheckedIdx = -1; }
		if (engine <= E_VAL) { for (int k = 0; k < SETUP_COUNT; ++k) S.val.stats[k] = SetupStats(); S.val.nextSignal = 0; S.val.totalR = 0; }
		if (engine <= E_LOG) { S.log.pending.clear(); S.log.lastQueuedIdx = -1; }
		++S.resetCount;
	}

	// Detects reloads / back-fills by comparing bar times at a few checkpoints.
	void CheckDataStamp(SCStudyInterfaceRef sc, ChartState& S)
	{
		const int n = sc.ArraySize;
		DataStamp& st = S.stamp;
		bool ok = st.valid && n > 0 && n >= st.arraySize - 0 && st.lastIdx < n;
		if (ok && st.lastIdx >= 0)
		{
			if (sc.BaseDateTimeIn[0].GetAsDouble() != st.t0) ok = false;
			else if (st.midIdx >= 0 && sc.BaseDateTimeIn[st.midIdx].GetAsDouble() != st.tMid) ok = false;
			else if (sc.BaseDateTimeIn[st.lastIdx].GetAsDouble() != st.tLast) ok = false;
		}
		else if (ok && st.lastIdx < 0 && st.arraySize > 0 && n > 0)
		{
			if (sc.BaseDateTimeIn[0].GetAsDouble() != st.t0) ok = false;
		}
		if (n < st.arraySize) ok = false;
		if (!ok)
		{
			ResetFrom(S, E_BASE);
			st.valid = (n > 0);
			st.arraySize = n; st.lastIdx = -1; st.midIdx = -1;
			st.t0 = n > 0 ? sc.BaseDateTimeIn[0].GetAsDouble() : 0; st.tMid = 0; st.tLast = 0;
		}
	}

	void UpdateDataStamp(SCStudyInterfaceRef sc, ChartState& S)
	{
		const int n = sc.ArraySize;
		DataStamp& st = S.stamp;
		st.arraySize = n;
		st.lastIdx = S.base.computedThrough;
		if (st.lastIdx >= 0)
		{
			st.tLast = sc.BaseDateTimeIn[st.lastIdx].GetAsDouble();
			st.midIdx = st.lastIdx / 2;
			st.tMid = sc.BaseDateTimeIn[st.midIdx].GetAsDouble();
		}
		st.t0 = n > 0 ? sc.BaseDateTimeIn[0].GetAsDouble() : 0;
		st.valid = n > 0;
	}

	// Store a parameter block; resets the engine (and dependents) when it changed.
	template<class T>
	void SetParams(ChartState& S, int engine, T& slot, const T& fresh)
	{
		if (!S.paramsSet[engine] || !SameBytes(slot, fresh))
		{
			slot = fresh;
			if (S.paramsSet[engine]) ResetFrom(S, engine);
			S.paramsSet[engine] = true;
		}
	}

	template<class V> inline void Fit(V& v, int n) { if (static_cast<int>(v.size()) != n) v.resize(static_cast<size_t>(n)); }

	// ==== 4  Base pass (ATR, sessions) ==========================================

	void EnsureBase(SCStudyInterfaceRef sc, ChartState& S)
	{
		const int n = sc.ArraySize;
		if (n <= 0) return;
		BaseState& B = S.base;
		Fit(B.atr, n); Fit(B.tr, n); Fit(B.tradingDay, n); Fit(B.isRth, n); Fit(B.rthSession, n);
		Fit(B.secIntoRth, n); Fit(B.dayStartIdx, n); Fit(B.rthStartIdx, n); Fit(B.lastRthId, n);
		if (B.UpToDate(sc)) return;
		int from = B.computedThrough + 1;
		if (from < 0) from = 0;
		if (from > n - 1) from = n - 1;
		const BaseParams& P = S.params.base;
		const int len = Max(1, P.atrLength);
		for (int i = from; i < n; ++i)
		{
			const float h = sc.High[i], l = sc.Low[i];
			const float pc = i > 0 ? sc.Close[i - 1] : sc.Open[i];
			float tr = h - l;
			tr = Max(tr, static_cast<float>(fabs(h - pc)));
			tr = Max(tr, static_cast<float>(fabs(l - pc)));
			B.tr[i] = tr;
			if (i == 0) B.atr[i] = tr;
			else if (i < len) B.atr[i] = (B.atr[i - 1] * i + tr) / (i + 1);
			else B.atr[i] = (B.atr[i - 1] * (len - 1) + tr) / len;
			if (B.atr[i] <= 0) B.atr[i] = Max(S.tickSize, tr);

			const SCDateTime& dt = sc.BaseDateTimeIn[i];
			B.tradingDay[i] = sc.GetTradingDayDate(dt);
			const int tod = dt.GetTimeInSeconds();
			const bool rth = (P.rthStartSec < P.rthEndSec) ? (tod >= P.rthStartSec && tod < P.rthEndSec) : (tod >= P.rthStartSec || tod < P.rthEndSec);
			B.isRth[i] = rth ? 1 : 0;
			const bool newDay = (i == 0) || (B.tradingDay[i] != B.tradingDay[i - 1]);
			B.dayStartIdx[i] = newDay ? i : B.dayStartIdx[i - 1];
			const int prevLastId = (i > 0) ? B.lastRthId[i - 1] : 0;
			if (rth)
			{
				const bool prevRthSameDay = (i > 0) && B.isRth[i - 1] && !newDay;
				if (!prevRthSameDay) { B.rthSession[i] = prevLastId + 1; B.rthStartIdx[i] = i; }
				else { B.rthSession[i] = B.rthSession[i - 1]; B.rthStartIdx[i] = B.rthStartIdx[i - 1]; }
				B.lastRthId[i] = B.rthSession[i];
				int sec = tod - P.rthStartSec; if (sec < 0) sec += 86400;
				B.secIntoRth[i] = sec;
			}
			else
			{
				B.rthSession[i] = -1;
				B.secIntoRth[i] = -1;
				B.rthStartIdx[i] = (i > 0 && !newDay) ? B.rthStartIdx[i - 1] : -1;
				B.lastRthId[i] = prevLastId;
			}
		}
		B.Stamp(sc);
		UpdateDataStamp(sc, S);
	}

	// Rolling helpers over base arrays
	inline float AtrAt(const ChartState& S, int i) { return (i >= 0 && i < static_cast<int>(S.base.atr.size())) ? S.base.atr[i] : S.tickSize * 10; }

	// ==== 5  Drawing helpers ====================================================

	inline bool DrawingAlive(SCStudyInterfaceRef sc, int& lineNumber)
	{
		if (lineNumber != 0 && sc.ChartDrawingExists(sc.ChartNumber, lineNumber) == 0) lineNumber = 0;
		return lineNumber != 0;
	}

	inline void DeleteDrawing(SCStudyInterfaceRef sc, int& lineNumber)
	{
		if (lineNumber != 0) { sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, lineNumber); lineNumber = 0; }
	}

	void DrawRect(SCStudyInterfaceRef sc, int& lineNumber, int beginIdx, int endIdx, float top, float bottom,
		uint32_t color, int transparency, const char* text = nullptr)
	{
		DrawingAlive(sc, lineNumber);
		s_UseTool T;
		T.ChartNumber = sc.ChartNumber; T.DrawingType = DRAWING_RECTANGLEHIGHLIGHT; T.Region = 0;
		T.AddMethod = UTAM_ADD_OR_ADJUST;
		if (lineNumber != 0) T.LineNumber = lineNumber;
		T.BeginIndex = Max(0, beginIdx); T.EndIndex = Max(beginIdx, endIdx);
		T.BeginValue = top; T.EndValue = bottom;
		T.Color = color; T.SecondaryColor = color; T.LineWidth = 1; T.TransparencyLevel = transparency;
		if (text) { T.Text = text; T.FontSize = 8; T.TextAlignment = DT_LEFT | DT_TOP; }
		if (sc.UseTool(T) > 0) lineNumber = T.LineNumber;
	}

	void DrawMarker(SCStudyInterfaceRef sc, int& lineNumber, int idx, float value, int markerType, int size, uint32_t color, int lineWidth = 2)
	{
		DrawingAlive(sc, lineNumber);
		s_UseTool T;
		T.ChartNumber = sc.ChartNumber; T.DrawingType = DRAWING_MARKER; T.Region = 0; T.AddMethod = UTAM_ADD_OR_ADJUST;
		if (lineNumber != 0) T.LineNumber = lineNumber;
		T.BeginIndex = idx; T.BeginValue = value; T.MarkerType = markerType; T.MarkerSize = size; T.Color = color; T.LineWidth = static_cast<uint16_t>(lineWidth);
		if (sc.UseTool(T) > 0) lineNumber = T.LineNumber;
	}

	void DrawSegment(SCStudyInterfaceRef sc, int& lineNumber, int beginIdx, int endIdx, float value, uint32_t color, int width, int style)
	{
		DrawingAlive(sc, lineNumber);
		s_UseTool T;
		T.ChartNumber = sc.ChartNumber; T.DrawingType = DRAWING_LINE; T.Region = 0; T.AddMethod = UTAM_ADD_OR_ADJUST;
		if (lineNumber != 0) T.LineNumber = lineNumber;
		T.BeginIndex = beginIdx; T.EndIndex = endIdx; T.BeginValue = value; T.EndValue = value;
		T.Color = color; T.LineWidth = static_cast<uint16_t>(width); T.LineStyle = static_cast<SubgraphLineStyles>(style);
		if (sc.UseTool(T) > 0) lineNumber = T.LineNumber;
	}

	void DrawRay(SCStudyInterfaceRef sc, int& lineNumber, int beginIdx, float value, uint32_t color, int width, int style, const char* text = nullptr)
	{
		DrawingAlive(sc, lineNumber);
		s_UseTool T;
		T.ChartNumber = sc.ChartNumber; T.DrawingType = DRAWING_HORIZONTAL_RAY; T.Region = 0; T.AddMethod = UTAM_ADD_OR_ADJUST;
		if (lineNumber != 0) T.LineNumber = lineNumber;
		T.BeginIndex = beginIdx; T.BeginValue = value; T.EndValue = value;
		T.Color = color; T.LineWidth = static_cast<uint16_t>(width); T.LineStyle = static_cast<SubgraphLineStyles>(style);
		if (text) { T.Text = text; T.FontSize = 8; T.ShowPrice = 0; }
		if (sc.UseTool(T) > 0) lineNumber = T.LineNumber;
	}

	void DrawLabel(SCStudyInterfaceRef sc, int& lineNumber, int idx, float value, const char* text, uint32_t color, int fontSize, bool bold)
	{
		DrawingAlive(sc, lineNumber);
		s_UseTool T;
		T.ChartNumber = sc.ChartNumber; T.DrawingType = DRAWING_TEXT; T.Region = 0; T.AddMethod = UTAM_ADD_OR_ADJUST;
		if (lineNumber != 0) T.LineNumber = lineNumber;
		T.BeginIndex = idx; T.BeginValue = value; T.Text = text; T.Color = color; T.FontSize = fontSize; T.FontBold = bold ? 1 : 0;
		T.TransparentLabelBackground = 1; T.TextAlignment = DT_LEFT | DT_BOTTOM;
		if (sc.UseTool(T) > 0) lineNumber = T.LineNumber;
	}

	void EnsureAuction(SCStudyInterfaceRef sc, ChartState& S);
	void EnsureVwap(SCStudyInterfaceRef sc, ChartState& S);
	void EnsureFlow(SCStudyInterfaceRef sc, ChartState& S);
	void EnsureRegime(SCStudyInterfaceRef sc, ChartState& S);
	// ==== 10 Intermarket (phase 5) ==============================================
	void EnsureInter(SCStudyInterfaceRef sc, ChartState& S);
	// ==== 11 DCS (phase 6) ======================================================
	void EnsureDcs(SCStudyInterfaceRef sc, ChartState& S);
	// ==== 12 Validation (phase 7) ===============================================
	void EnsureVal(SCStudyInterfaceRef sc, ChartState& S);
	// ==== 13 Logger (phase 7) ===================================================
	void EnsureLog(SCStudyInterfaceRef sc, ChartState& S);

	// ==== 6  Auction / Structure engine ========================================
	namespace auction_detail
	{
		typedef std::map<int, double> LevelMap;

		inline int PriceToTick(float price, float tick) { return static_cast<int>(floor(price / tick + 0.5)); }
		inline int TickToLevel(int t, int tpl) { return (t >= 0 ? t : t - tpl + 1) / tpl; }
		inline float LevelBottom(int level, float tick, int tpl) { return (static_cast<float>(level) * tpl - 0.5f) * tick; }
		inline float LevelTop(int level, float tick, int tpl) { return (static_cast<float>(level) * tpl + tpl - 0.5f) * tick; }
		inline float LevelMid(int level, float tick, int tpl) { return (static_cast<float>(level) * tpl + (tpl - 1) * 0.5f) * tick; }

		// Adds bar i's volume-at-price to the map: real VAP when available, else bar volume spread over H..L.
		// Returns the set of levels touched (for TPO counting) through r_lo/r_hi.
		void AddBarToProfile(SCStudyInterfaceRef sc, ChartState& S, int i, LevelMap& vol, double& total, int& r_lo, int& r_hi)
		{
			const int tpl = Max(1, S.params.auction.profileTicksPerLevel);
			const float tick = S.tickSize;
			r_lo = TickToLevel(PriceToTick(sc.Low[i], tick), tpl);
			r_hi = TickToLevel(PriceToTick(sc.High[i], tick), tpl);
			bool used = false;
			if (sc.VolumeAtPriceForBars != nullptr && static_cast<int>(sc.VolumeAtPriceForBars->GetNumberOfBars()) > i)
			{
				const int cnt = sc.VolumeAtPriceForBars->GetSizeAtBarIndex(i);
				if (cnt > 0)
				{
					used = true;
					const s_VolumeAtPriceV2* p = nullptr;
					for (int k = 0; k < cnt; ++k)
					{
						if (!sc.VolumeAtPriceForBars->GetVAPElementAtIndex(i, k, &p) || p == nullptr) break;
						const int level = TickToLevel(p->PriceInTicks, tpl);
						vol[level] += p->Volume; total += p->Volume;
					}
				}
			}
			if (!used)
			{
				const double v = sc.Volume[i];
				const int levels = Max(1, r_hi - r_lo + 1);
				for (int lv = r_lo; lv <= r_hi; ++lv) vol[lv] += v / levels;
				total += v;
			}
		}

		// POC and value area (two-levels-at-a-time expansion from the POC).
		bool ComputePocVa(const LevelMap& vol, double total, float vaPct, int& poc, int& vah, int& val)
		{
			if (vol.empty() || total <= 0) return false;
			std::vector<std::pair<int, double> > v(vol.begin(), vol.end());
			size_t pocIdx = 0; double best = -1;
			for (size_t k = 0; k < v.size(); ++k) if (v[k].second > best) { best = v[k].second; pocIdx = k; }
			// tie: prefer the level nearest the middle of the range
			{
				const size_t mid = v.size() / 2;
				for (size_t k = 0; k < v.size(); ++k)
					if (v[k].second == best && (k > mid ? k - mid : mid - k) < (pocIdx > mid ? pocIdx - mid : mid - pocIdx)) pocIdx = k;
			}
			poc = v[pocIdx].first;
			const double target = total * vaPct / 100.0;
			double acc = v[pocIdx].second;
			size_t up = pocIdx, dn = pocIdx;
			while (acc < target && (up + 1 < v.size() || dn > 0))
			{
				double upSum = 0, dnSum = 0;
				if (up + 1 < v.size()) { upSum = v[up + 1].second; if (up + 2 < v.size()) upSum += v[up + 2].second; }
				if (dn > 0) { dnSum = v[dn - 1].second; if (dn > 1) dnSum += v[dn - 2].second; }
				if ((upSum >= dnSum && up + 1 < v.size()) || dn == 0)
				{
					acc += v[up + 1].second; ++up;
					if (up + 1 < v.size() && acc < target) { acc += v[up + 1].second; ++up; }
				}
				else
				{
					acc += v[dn - 1].second; --dn;
					if (dn > 0 && acc < target) { acc += v[dn - 1].second; --dn; }
				}
			}
			vah = v[up].first; val = v[dn].first;
			return true;
		}

		inline bool IsPivotHigh(SCStudyInterfaceRef sc, int p, int N)
		{
			const float h = sc.High[p];
			for (int k = 1; k <= N; ++k) if (h <= sc.High[p - k] || h <= sc.High[p + k]) return false;
			return true;
		}
		inline bool IsPivotLow(SCStudyInterfaceRef sc, int p, int N)
		{
			const float l = sc.Low[p];
			for (int k = 1; k <= N; ++k) if (l >= sc.Low[p - k] || l >= sc.Low[p + k]) return false;
			return true;
		}

		void RefreshSwingIndices(AuctionState& A)
		{
			A.lastSwingHigh = A.prevSwingHigh = A.lastSwingLow = A.prevSwingLow = -1;
			for (int k = static_cast<int>(A.swings.size()) - 1; k >= 0; --k)
			{
				const Swing& s = A.swings[k];
				if (s.high) { if (A.lastSwingHigh < 0) A.lastSwingHigh = k; else if (A.prevSwingHigh < 0) A.prevSwingHigh = k; }
				else { if (A.lastSwingLow < 0) A.lastSwingLow = k; else if (A.prevSwingLow < 0) A.prevSwingLow = k; }
				if (A.prevSwingHigh >= 0 && A.prevSwingLow >= 0) break;
			}
		}

		// Returns true when the swing list changed (new or replaced extreme).
		bool AddSwing(AuctionState& A, const Swing& s, float minDist, bool& r_replaced)
		{
			r_replaced = false;
			if (!A.swings.empty())
			{
				Swing& last = A.swings.back();
				if (last.high == s.high)
				{
					if ((s.high && s.price > last.price) || (!s.high && s.price < last.price)) { last = s; r_replaced = true; RefreshSwingIndices(A); return true; }
					return false;
				}
				if (fabs(s.price - last.price) < minDist) return false;
			}
			A.swings.push_back(s);
			if (A.swings.size() > 400) A.swings.erase(A.swings.begin(), A.swings.begin() + 100);
			RefreshSwingIndices(A);
			return true;
		}

		// Rebuilds single-print zones from the committed TPO map (count == 1, excluding the tails).
		void RebuildSinglePrints(ChartState& S, int bornIdx)
		{
			AuctionState& A = S.auction;
			const int tpl = Max(1, S.params.auction.profileTicksPerLevel);
			std::vector<Zone> fresh;
			if (A.tpoMap.size() >= 3)
			{
				std::vector<std::pair<int, int> > v(A.tpoMap.begin(), A.tpoMap.end());
				size_t lo = 0, hi = v.size() - 1;
				while (lo < v.size() && v[lo].second <= 1) ++lo;           // lower tail
				while (hi > lo && v[hi].second <= 1) --hi;                 // upper tail
				size_t k = lo;
				while (k <= hi && k < v.size())
				{
					if (v[k].second == 1)
					{
						size_t e = k; while (e + 1 <= hi && v[e + 1].second == 1 && v[e + 1].first == v[e].first + 1) ++e;
						Zone z; z.bornIdx = bornIdx; z.bottom = LevelBottom(v[k].first, S.tickSize, tpl); z.top = LevelTop(v[e].first, S.tickSize, tpl);
						z.kind = LVL_SINGLE_PRINT; z.dir = 0; z.active = true;
						fresh.push_back(z);
						k = e + 1;
					}
					else ++k;
				}
			}
			// keep line numbers of zones that still exist (same top/bottom)
			for (size_t a = 0; a < fresh.size(); ++a)
				for (size_t b = 0; b < A.singlePrints.size(); ++b)
					if (A.singlePrints[b].lineNumber != 0 && fabs(A.singlePrints[b].top - fresh[a].top) < 1e-6 && fabs(A.singlePrints[b].bottom - fresh[a].bottom) < 1e-6)
					{ fresh[a].lineNumber = A.singlePrints[b].lineNumber; fresh[a].bornIdx = A.singlePrints[b].bornIdx; A.singlePrints[b].lineNumber = 0; break; }
			for (size_t b = 0; b < A.singlePrints.size(); ++b) if (A.singlePrints[b].lineNumber != 0) A.deadLines.push_back(A.singlePrints[b].lineNumber);
			A.singlePrints.swap(fresh);
		}

		void FinalizeRthSession(ChartState& S, int atIdx)
		{
			AuctionState& A = S.auction;
			if (A.profTotal > 0)
			{
				int poc, vah, val;
				if (ComputePocVa(A.profVol, A.profTotal, S.params.auction.valueAreaPct, poc, vah, val))
				{
					const int tpl = Max(1, S.params.auction.profileTicksPerLevel);
					A.prevPoc = LevelMid(poc, S.tickSize, tpl); A.prevVah = LevelTop(vah, S.tickSize, tpl); A.prevVal = LevelBottom(val, S.tickSize, tpl);
					A.nakedPocs.push_back(A.prevPoc); A.nakedPocBorn.push_back(atIdx); A.nakedPocLine.push_back(0);
					const int keep = Max(0, S.params.auction.nakedPocsTracked);
					while (static_cast<int>(A.nakedPocs.size()) > keep)
					{
						if (A.nakedPocLine[0] != 0) A.deadLines.push_back(A.nakedPocLine[0]);
						A.nakedPocs.erase(A.nakedPocs.begin()); A.nakedPocBorn.erase(A.nakedPocBorn.begin()); A.nakedPocLine.erase(A.nakedPocLine.begin());
					}
				}
			}
			if (A.rthHigh > -FLT_MAX) { A.prevDayHigh = A.rthHigh; A.prevDayLow = A.rthLow; }
			if (A.ibClosed && A.ibH > -FLT_MAX) A.ibRangeHistory.push_back(A.ibH - A.ibL);
			if (A.ibRangeHistory.size() > 200) A.ibRangeHistory.erase(A.ibRangeHistory.begin());
			// close the TPO period and clear the session profile
			A.profVol.clear(); A.profTotal = 0; A.tpoMap.clear(); A.periodLevels.clear(); A.lastTpoPeriod = -1;
			for (size_t b = 0; b < A.singlePrints.size(); ++b) if (A.singlePrints[b].lineNumber != 0) A.deadLines.push_back(A.singlePrints[b].lineNumber);
			A.singlePrints.clear();
			A.rthHigh = -FLT_MAX; A.rthLow = FLT_MAX; A.ibH = -FLT_MAX; A.ibL = FLT_MAX; A.ibClosed = false; A.ibCloseIdx = -1;
			A.rthOpen = 0; A.rthOpenIdx = -1; A.todayOpenType = OT_NONE; A.openTypeDone = false;
			A.o30High = -FLT_MAX; A.o30Low = FLT_MAX; A.o30HighIdx = -1; A.o30LowIdx = -1;
		}

		int ClassifyOpen(const AuctionState& A, float atr, float close30, float& r_dir)
		{
			r_dir = 0;
			const float O = A.rthOpen, H = A.o30High, L = A.o30Low;
			const float range = H - L;
			if (range <= 0 || atr <= 0) return OT_NONE;
			const float openPos = (O - L) / range;            // 0 = open at low, 1 = open at high
			const float closePos = (close30 - L) / range;
			const bool lowFirst = A.o30LowIdx < A.o30HighIdx;
			// Open-Drive: open at one extreme, close at the other end
			if (openPos <= 0.15f && closePos >= 0.7f) { r_dir = 1.0f; return OT_DRIVE_UP; }
			if (openPos >= 0.85f && closePos <= 0.3f) { r_dir = -1.0f; return OT_DRIVE_DOWN; }
			// Open-Test-Drive: shallow test against, then drive and close at the extreme
			if (openPos > 0.15f && openPos <= 0.35f && lowFirst && closePos >= 0.7f) { r_dir = 0.7f; return OT_TEST_DRIVE_UP; }
			if (openPos < 0.85f && openPos >= 0.65f && !lowFirst && closePos <= 0.3f) { r_dir = -0.7f; return OT_TEST_DRIVE_DOWN; }
			// Open-Rejection-Reverse: deep move first, then back through the open
			if (openPos >= 0.35f && lowFirst && closePos > openPos + 0.15f) { r_dir = 0.5f; return OT_REJECT_REVERSE_UP; }
			if (openPos <= 0.65f && !lowFirst && closePos < openPos - 0.15f) { r_dir = -0.5f; return OT_REJECT_REVERSE_DOWN; }
			r_dir = 0; return OT_AUCTION;
		}
	}

	void EnsureAuction(SCStudyInterfaceRef sc, ChartState& S)
	{
		using namespace auction_detail;
		EnsureBase(sc, S);
		const int n = sc.ArraySize; if (n <= 0) return;
		AuctionState& A = S.auction; const AuctionParams& P = S.params.auction; const BaseState& B = S.base;
		Fit(A.poc, n); Fit(A.vah, n); Fit(A.val, n); Fit(A.pdPoc, n); Fit(A.pdVah, n); Fit(A.pdVal, n); Fit(A.pdh, n); Fit(A.pdl, n);
		Fit(A.onHigh, n); Fit(A.onLow, n); Fit(A.ibHigh, n); Fit(A.ibLow, n); Fit(A.ibDone, n);
		Fit(A.structTrend, n); Fit(A.bos, n); Fit(A.vaPos, n); Fit(A.pocPos, n); Fit(A.ibPos, n); Fit(A.valueMig, n); Fit(A.openTypeDir, n);
		Fit(A.openType, n); Fit(A.swingHighMark, n); Fit(A.swingLowMark, n); Fit(A.bosMark, n); Fit(A.chochMark, n);
		if (A.UpToDate(sc)) return;   // nothing new since the last Ensure in this update cycle
		int from = A.computedThrough + 1; if (from < 0) from = 0; if (from > n - 1) from = n - 1;
		const int tpl = Max(1, P.profileTicksPerLevel);
		const float tick = S.tickSize;
		const int N = Max(2, P.swingStrength);

		for (int i = from; i < n; ++i)
		{
			const bool closed = (i <= n - 2);
			const float atr = AtrAt(S, i);
			const float c = sc.Close[i], h = sc.High[i], l = sc.Low[i];
			const bool rth = B.isRth[i] != 0;
			const bool newDay = (i == 0) || (B.tradingDay[i] != B.tradingDay[i - 1]);
			const bool prevRth = (i > 0) && B.isRth[i - 1] != 0;

			// ---- session transitions (idempotent thanks to the guards) ----
			if (prevRth && (!rth || newDay) && A.finalizedSession != B.rthSession[i - 1]) { A.finalizedSession = B.rthSession[i - 1]; FinalizeRthSession(S, i); }
			if (newDay && A.curDay != B.tradingDay[i])
			{
				A.curDay = B.tradingDay[i];
				A.onH = -FLT_MAX; A.onL = FLT_MAX;
			}
			if (rth && !prevRth && A.rthOpenIdx != i && A.rthOpenSession != B.rthSession[i])
			{
				A.rthOpenSession = B.rthSession[i]; A.rthOpen = sc.Open[i]; A.rthOpenIdx = i; A.todayOpenType = OT_NONE; A.openTypeDone = false;
				A.o30High = -FLT_MAX; A.o30Low = FLT_MAX; A.o30HighIdx = A.o30LowIdx = -1;
				A.profVol.clear(); A.profTotal = 0; A.tpoMap.clear(); A.periodLevels.clear(); A.lastTpoPeriod = -1;
				A.rthHigh = -FLT_MAX; A.rthLow = FLT_MAX; A.ibH = -FLT_MAX; A.ibL = FLT_MAX; A.ibClosed = false; A.ibCloseIdx = -1;
			}

			// ---- provisional copies of the committed accumulators for this bar ----
			float onH = A.onH, onL = A.onL, ibH = A.ibH, ibL = A.ibL, rthHigh = A.rthHigh, rthLow = A.rthLow;
			bool ibClosed = A.ibClosed;
			if (!rth && B.rthStartIdx[i] < 0)
			{
				// overnight: this trading day has not opened its RTH session yet
				onH = Max(onH, h); onL = Min(onL, l);
				if (closed) { A.onH = onH; A.onL = onL; }
			}
			if (rth)
			{
				rthHigh = Max(rthHigh, h); rthLow = Min(rthLow, l);
				const bool inIb = B.secIntoRth[i] < P.ibMinutes * 60;
				if (inIb) { ibH = Max(ibH, h); ibL = Min(ibL, l); }
				else if (!ibClosed && ibH > -FLT_MAX) { ibClosed = true; }
				if (closed) { A.rthHigh = rthHigh; A.rthLow = rthLow; A.ibH = ibH; A.ibL = ibL; if (ibClosed && !A.ibClosed) { A.ibClosed = true; A.ibCloseIdx = i; } }
			}

			// ---- profile (committed on closed bars; forming bar evaluated on a copy) ----
			float poc = 0, vah = 0, val = 0;
			if (rth)
			{
				int lo = 0, hi = 0;
				if (closed)
				{
					AddBarToProfile(sc, S, i, A.profVol, A.profTotal, lo, hi);
					const int period = B.secIntoRth[i] / Max(60, P.tpoMinutes * 60);
					if (A.lastTpoPeriod >= 0 && period != A.lastTpoPeriod)
					{
						// commit the period that just finished (levels touched by its closed bars)
						for (std::set<int>::const_iterator it = A.periodLevels.begin(); it != A.periodLevels.end(); ++it) A.tpoMap[*it] += 1;
						A.periodLevels.clear();
						RebuildSinglePrints(S, i);
					}
					for (int lv = lo; lv <= hi; ++lv) A.periodLevels.insert(lv);
					A.lastTpoPeriod = period;
					int p, vh, vl;
					if (ComputePocVa(A.profVol, A.profTotal, P.valueAreaPct, p, vh, vl)) { poc = LevelMid(p, tick, tpl); vah = LevelTop(vh, tick, tpl); val = LevelBottom(vl, tick, tpl); }
				}
				else
				{
					LevelMap tmp(A.profVol); double tot = A.profTotal;
					AddBarToProfile(sc, S, i, tmp, tot, lo, hi);
					int p, vh, vl;
					if (ComputePocVa(tmp, tot, P.valueAreaPct, p, vh, vl)) { poc = LevelMid(p, tick, tpl); vah = LevelTop(vh, tick, tpl); val = LevelBottom(vl, tick, tpl); }
				}
				A.devPoc = poc; A.devVah = vah; A.devVal = val;
			}

			// ---- open type after the first N minutes ----
			if (rth && A.rthOpenIdx >= 0)
			{
				const int evalSec = P.openTypeMinutes * 60;
				if (B.secIntoRth[i] < evalSec)
				{
					if (closed)
					{
						if (h > A.o30High) { A.o30High = h; A.o30HighIdx = i; }
						if (l < A.o30Low) { A.o30Low = l; A.o30LowIdx = i; }
					}
				}
				else if (!A.openTypeDone && closed && A.o30HighIdx >= 0)
				{
					float dir = 0; A.todayOpenType = static_cast<signed char>(ClassifyOpen(A, atr, sc.Close[i - 1], dir)); A.openTypeDirValue = dir; A.openTypeDone = true;
				}
			}

			// ---- swings (confirmed N bars late, closed bars only) ----
			bool swingsChanged = false;
			if (closed && i - 2 * N >= 0)
			{
				const int p = i - N;
				const float minDist = P.swingMinAtr * AtrAt(S, p);
				bool replaced = false;
				if (IsPivotHigh(sc, p, N)) { Swing s; s.idx = p; s.price = sc.High[p]; s.confirmIdx = i; s.high = true; if (AddSwing(A, s, minDist, replaced)) { swingsChanged = true; A.lastHighBroken = false; A.swingHighMark[p] = 1; A.dirtyFrom = Min(A.dirtyFrom, p); } }
				if (IsPivotLow(sc, p, N)) { Swing s; s.idx = p; s.price = sc.Low[p]; s.confirmIdx = i; s.high = false; if (AddSwing(A, s, minDist, replaced)) { swingsChanged = true; A.lastLowBroken = false; A.swingLowMark[p] = 1; A.dirtyFrom = Min(A.dirtyFrom, p); } }
				if (swingsChanged)
				{
					// liquidity pools: equal highs / lows within tolerance
					const float tol = P.equalTolTicks * tick;
					const Swing& ns = A.swings.back();
					for (int k = static_cast<int>(A.swings.size()) - 2; k >= 0 && k >= static_cast<int>(A.swings.size()) - 8; --k)
					{
						const Swing& q = A.swings[k];
						if (q.high != ns.high || fabs(q.price - ns.price) > tol) continue;
						bool merged = false;
						for (size_t z = 0; z < A.liquidity.size(); ++z)
						{
							Zone& Z = A.liquidity[z];
							if (!Z.active || (Z.kind == LVL_LIQ_EQH) != ns.high) continue;
							if (ns.price >= Z.bottom - tol && ns.price <= Z.top + tol) { Z.top = Max(Z.top, ns.price); Z.bottom = Min(Z.bottom, ns.price); merged = true; break; }
						}
						if (!merged)
						{
							Zone Z; Z.bornIdx = q.idx; Z.top = Max(q.price, ns.price); Z.bottom = Min(q.price, ns.price); Z.kind = ns.high ? LVL_LIQ_EQH : LVL_LIQ_EQL; Z.dir = ns.high ? -1 : 1; Z.active = true;
							A.liquidity.push_back(Z);
						}
						break;
					}
					while (static_cast<int>(A.liquidity.size()) > 60) { if (A.liquidity[0].lineNumber) A.deadLines.push_back(A.liquidity[0].lineNumber); A.liquidity.erase(A.liquidity.begin()); }
				}
			}

			// structure trend
			float st = 0;
			if (A.lastSwingHigh >= 0 && A.prevSwingHigh >= 0 && A.lastSwingLow >= 0 && A.prevSwingLow >= 0)
			{
				const bool hh = A.swings[A.lastSwingHigh].price > A.swings[A.prevSwingHigh].price;
				const bool hl = A.swings[A.lastSwingLow].price > A.swings[A.prevSwingLow].price;
				if (hh && hl) st = 1; else if (!hh && !hl) st = -1; else st = 0;
			}

			// BOS / CHoCH on closed bars
			if (closed)
			{
				const float prevTrend = (i > 0) ? A.structTrend[i - 1] : 0;
				if (A.lastSwingHigh >= 0 && !A.lastHighBroken && A.swings[A.lastSwingHigh].idx < i && c > A.swings[A.lastSwingHigh].price)
				{
					A.lastHighBroken = true;
					if (prevTrend < 0) { A.chochMark[i] = 1; A.lastChochDir = 1; A.lastChochIdx = i; } else { A.bosMark[i] = 1; }
					A.lastBosDir = 1; A.lastBosIdx = i;
				}
				if (A.lastSwingLow >= 0 && !A.lastLowBroken && A.swings[A.lastSwingLow].idx < i && c < A.swings[A.lastSwingLow].price)
				{
					A.lastLowBroken = true;
					if (prevTrend > 0) { A.chochMark[i] = -1; A.lastChochDir = -1; A.lastChochIdx = i; } else { A.bosMark[i] = -1; }
					A.lastBosDir = -1; A.lastBosIdx = i;
				}
				// liquidity pools die when a close goes through them
				for (size_t z = 0; z < A.liquidity.size(); ++z)
				{
					Zone& Z = A.liquidity[z]; if (!Z.active) continue;
					if ((Z.kind == LVL_LIQ_EQH && c > Z.top) || (Z.kind == LVL_LIQ_EQL && c < Z.bottom)) { Z.active = false; Z.deadIdx = i; }
				}
				// naked POCs tested?
				for (size_t k = 0; k < A.nakedPocs.size(); )
				{
					if (l <= A.nakedPocs[k] && h >= A.nakedPocs[k] && i > A.nakedPocBorn[k])
					{
						if (A.nakedPocLine[k] != 0) A.deadLines.push_back(A.nakedPocLine[k]);
						A.nakedPocs.erase(A.nakedPocs.begin() + k); A.nakedPocBorn.erase(A.nakedPocBorn.begin() + k); A.nakedPocLine.erase(A.nakedPocLine.begin() + k);
					}
					else ++k;
				}
			}

			// ---- outputs ----
			A.structTrend[i] = st;
			const float decay = static_cast<float>(Max(1, P.bosDecayBars));
			A.bos[i] = (A.lastBosIdx >= 0 && i >= A.lastBosIdx) ? static_cast<float>(A.lastBosDir) * static_cast<float>(exp(-(i - A.lastBosIdx) / decay)) : 0.0f;
			A.poc[i] = poc; A.vah[i] = vah; A.val[i] = val;
			A.pdPoc[i] = A.prevPoc; A.pdVah[i] = A.prevVah; A.pdVal[i] = A.prevVal; A.pdh[i] = A.prevDayHigh; A.pdl[i] = A.prevDayLow;
			A.onHigh[i] = onH > -FLT_MAX ? onH : 0.0f; A.onLow[i] = onL < FLT_MAX ? onL : 0.0f;
			A.ibHigh[i] = (rth && ibH > -FLT_MAX) ? ibH : 0.0f; A.ibLow[i] = (rth && ibL < FLT_MAX) ? ibL : 0.0f; A.ibDone[i] = (rth && ibClosed) ? 1 : 0;
			// location features
			if (vah > val && atr > 0)
			{
				if (c > vah) A.vaPos[i] = Clamp1(0.5 + 0.5 * (c - vah) / atr);
				else if (c < val) A.vaPos[i] = Clamp1(-0.5 - 0.5 * (val - c) / atr);
				else A.vaPos[i] = Clamp1(((c - val) / (vah - val) - 0.5) * 1.0);
				A.pocPos[i] = Clamp1((c - poc) / (2.0 * atr));
			}
			else { A.vaPos[i] = (i > 0) ? A.vaPos[i - 1] * 0.9f : 0.0f; A.pocPos[i] = (i > 0) ? A.pocPos[i - 1] * 0.9f : 0.0f; }
			if (rth && ibClosed && ibH > ibL && atr > 0)
			{
				const float mid = 0.5f * (ibH + ibL);
				if (c > ibH) A.ibPos[i] = Clamp1(0.5 + 0.5 * (c - ibH) / atr);
				else if (c < ibL) A.ibPos[i] = Clamp1(-0.5 - 0.5 * (ibL - c) / atr);
				else A.ibPos[i] = Clamp1((c - mid) / (ibH - ibL));
			}
			else A.ibPos[i] = 0;
			// value migration (needs a developing VA and a prior-day VA)
			if (rth && vah > val && A.prevVah > A.prevVal && B.secIntoRth[i] >= 1800)
			{
				const float ov = Max(0.0f, Min(vah, A.prevVah) - Max(val, A.prevVal));
				const float w = Min(vah - val, A.prevVah - A.prevVal);
				if (w > 0 && ov / w >= 0.5f) A.valueMig[i] = 0; else A.valueMig[i] = Sign(poc - A.prevPoc);
			}
			else A.valueMig[i] = (i > 0 && rth) ? A.valueMig[i - 1] : 0.0f;
			A.openType[i] = rth ? A.todayOpenType : static_cast<signed char>(OT_NONE);
			A.openTypeDir[i] = (rth && A.openTypeDone && B.secIntoRth[i] <= 2 * 3600) ? A.openTypeDirValue : 0.0f;
		}
		A.Stamp(sc);

		// ---- level snapshot for HUD / targets (auction part) ----
		A.levels.clear();
		const int li = n - 1;
		const float pc = sc.Close[li];
		if (A.poc[li] > 0) { A.levels.push_back({ A.poc[li], LVL_POC, li }); A.levels.push_back({ A.vah[li], LVL_VAH, li }); A.levels.push_back({ A.val[li], LVL_VAL, li }); }
		if (A.pdPoc[li] > 0) { A.levels.push_back({ A.pdPoc[li], LVL_PD_POC, li }); A.levels.push_back({ A.pdVah[li], LVL_PD_VAH, li }); A.levels.push_back({ A.pdVal[li], LVL_PD_VAL, li }); }
		if (A.pdh[li] > 0) { A.levels.push_back({ A.pdh[li], LVL_PDH, li }); A.levels.push_back({ A.pdl[li], LVL_PDL, li }); }
		if (A.onHigh[li] > 0) { A.levels.push_back({ A.onHigh[li], LVL_ONH, li }); A.levels.push_back({ A.onLow[li], LVL_ONL, li }); }
		if (A.ibHigh[li] > 0 && A.ibDone[li])
		{
			const float r = A.ibHigh[li] - A.ibLow[li];
			A.levels.push_back({ A.ibHigh[li], LVL_IBH, li }); A.levels.push_back({ A.ibLow[li], LVL_IBL, li });
			const float m[3] = { P.ibExtA, P.ibExtB, P.ibExtC };
			for (int k = 0; k < 3; ++k) if (m[k] > 0) { A.levels.push_back({ A.ibHigh[li] + r * m[k], LVL_IBEXT, li }); A.levels.push_back({ A.ibLow[li] - r * m[k], LVL_IBEXT, li }); }
		}
		for (size_t k = 0; k < A.nakedPocs.size(); ++k) A.levels.push_back({ A.nakedPocs[k], LVL_NAKED_POC, A.nakedPocBorn[k] });
		if (A.lastSwingHigh >= 0) A.levels.push_back({ A.swings[A.lastSwingHigh].price, LVL_SWING_H, A.swings[A.lastSwingHigh].idx });
		if (A.lastSwingLow >= 0) A.levels.push_back({ A.swings[A.lastSwingLow].price, LVL_SWING_L, A.swings[A.lastSwingLow].idx });
		for (size_t z = 0; z < A.liquidity.size(); ++z) if (A.liquidity[z].active) A.levels.push_back({ 0.5f * (A.liquidity[z].top + A.liquidity[z].bottom), A.liquidity[z].kind, A.liquidity[z].bornIdx });
		(void)pc;
	}

	// ==== 7  VWAP engine ========================================================
	namespace vwap_detail
	{
		inline float Tp(SCStudyInterfaceRef sc, int i) { return (sc.High[i] + sc.Low[i] + sc.Close[i]) / 3.0f; }

		// Anchored VWAP sums from bar a to bar b inclusive.
		inline void SumRange(SCStudyInterfaceRef sc, int a, int b, double& pv, double& v)
		{
			pv = 0; v = 0;
			for (int k = Max(0, a); k <= b; ++k) { const double vol = sc.Volume[k]; pv += Tp(sc, k) * vol; v += vol; }
		}
	}

	void EnsureVwap(SCStudyInterfaceRef sc, ChartState& S)
	{
		using namespace vwap_detail;
		EnsureAuction(sc, S);
		const int n = sc.ArraySize; if (n <= 0) return;
		VwapState& V = S.vwap; const VwapParams& P = S.params.vwap; const BaseState& B = S.base; const AuctionState& A = S.auction;
		Fit(V.vwap, n); Fit(V.sd, n); Fit(V.b1u, n); Fit(V.b1d, n); Fit(V.b2u, n); Fit(V.b2d, n); Fit(V.b3u, n); Fit(V.b3d, n);
		Fit(V.avOn, n); Fit(V.avRth, n); Fit(V.avSwHi, n); Fit(V.avSwLo, n); Fit(V.slope, n); Fit(V.pos, n); Fit(V.accept, n);
		if (V.UpToDate(sc)) return;
		int from = V.computedThrough + 1; if (from < 0) from = 0; if (from > n - 1) from = n - 1;
		const int k = Max(1, P.slopeBars);

		for (int i = from; i < n; ++i)
		{
			const bool closed = (i <= n - 2);
			const float atr = AtrAt(S, i);
			const double vol = sc.Volume[i];
			const double tp = Tp(sc, i);
			const double pv = tp * vol, ppv = tp * tp * vol;
			const bool rth = B.isRth[i] != 0;

			// --- session VWAP (anchor: RTH open or trading-day start) ---
			int sessionKey = (P.anchor == 0) ? (rth ? B.rthSession[i] : -1) : B.tradingDay[i];
			if (P.anchor == 0 && !rth) sessionKey = -1;
			if (sessionKey != V.sSession) { V.sPV = V.sPPV = V.sV = 0; V.sSession = sessionKey; V.sStartIdx = i; V.acceptRun = 0; }
			double sPV = V.sPV + pv, sPPV = V.sPPV + ppv, sV = V.sV + vol;
			float vwap = 0, sd = 0;
			if (sessionKey >= 0 && sV > 0)
			{
				vwap = static_cast<float>(sPV / sV);
				const double var = sPPV / sV - static_cast<double>(vwap) * vwap;
				sd = var > 0 ? static_cast<float>(sqrt(var)) : 0.0f;
			}
			if (closed) { V.sPV = sPV; V.sPPV = sPPV; V.sV = sV; }
			V.vwap[i] = vwap; V.sd[i] = sd;
			V.b1u[i] = vwap > 0 ? vwap + P.band1 * sd : 0; V.b1d[i] = vwap > 0 ? vwap - P.band1 * sd : 0;
			V.b2u[i] = vwap > 0 ? vwap + P.band2 * sd : 0; V.b2d[i] = vwap > 0 ? vwap - P.band2 * sd : 0;
			V.b3u[i] = vwap > 0 ? vwap + P.band3 * sd : 0; V.b3d[i] = vwap > 0 ? vwap - P.band3 * sd : 0;

			// --- overnight-open anchored (trading day start) ---
			if (B.tradingDay[i] != V.oDay) { V.oPV = V.oV = 0; V.oDay = B.tradingDay[i]; }
			{ const double a = V.oPV + pv, b = V.oV + vol; V.avOn[i] = b > 0 ? static_cast<float>(a / b) : 0.0f; if (closed) { V.oPV = a; V.oV = b; } }

			// --- RTH-open anchored ---
			{
				const int key = rth ? B.rthSession[i] : -1;
				if (key != V.rSession) { V.rPV = V.rV = 0; V.rSession = key; }
				const double a = V.rPV + pv, b = V.rV + vol;
				V.avRth[i] = (key >= 0 && b > 0) ? static_cast<float>(a / b) : 0.0f;
				if (closed) { V.rPV = a; V.rV = b; }
			}

			// --- swing-anchored (last confirmed swing high / low) ---
			{
				const int hIdx = A.lastSwingHigh >= 0 ? A.swings[A.lastSwingHigh].idx : -1;
				const int lIdx = A.lastSwingLow >= 0 ? A.swings[A.lastSwingLow].idx : -1;
				if (hIdx != V.hAnchor)
				{
					V.hAnchor = hIdx;
					if (hIdx >= 0)
					{
						// back-fill from the pivot bar up to the last committed bar, then continue incrementally
						double cpv = 0, cv = 0;
						for (int j = hIdx; j < i; ++j) { const double vv = sc.Volume[j]; cpv += Tp(sc, j) * vv; cv += vv; V.avSwHi[j] = cv > 0 ? static_cast<float>(cpv / cv) : 0.0f; }
						V.hPV = cpv; V.hV = cv; V.dirtyFrom = Min(V.dirtyFrom, hIdx);
					}
					else { V.hPV = V.hV = 0; }
				}
				if (lIdx != V.lAnchor)
				{
					V.lAnchor = lIdx;
					if (lIdx >= 0)
					{
						double cpv = 0, cv = 0;
						for (int j = lIdx; j < i; ++j) { const double vv = sc.Volume[j]; cpv += Tp(sc, j) * vv; cv += vv; V.avSwLo[j] = cv > 0 ? static_cast<float>(cpv / cv) : 0.0f; }
						V.lPV = cpv; V.lV = cv; V.dirtyFrom = Min(V.dirtyFrom, lIdx);
					}
					else { V.lPV = V.lV = 0; }
				}
				if (hIdx >= 0) { const double a = V.hPV + pv, b = V.hV + vol; V.avSwHi[i] = b > 0 ? static_cast<float>(a / b) : 0.0f; if (closed) { V.hPV = a; V.hV = b; } } else V.avSwHi[i] = 0;
				if (lIdx >= 0) { const double a = V.lPV + pv, b = V.lV + vol; V.avSwLo[i] = b > 0 ? static_cast<float>(a / b) : 0.0f; if (closed) { V.lPV = a; V.lV = b; } } else V.avSwLo[i] = 0;
			}

			// --- features ---
			const float c = sc.Close[i];
			if (vwap > 0 && i - k >= 0 && V.vwap[i - k] > 0 && atr > 0 && (i - k) >= V.sStartIdx)
				V.slope[i] = Clamp1(((vwap - V.vwap[i - k]) / atr) / 0.5);
			else V.slope[i] = 0;
			if (vwap > 0) { const float scale = sd > 0.25f * atr ? 2.0f * sd : Max(atr, S.tickSize); V.pos[i] = Clamp1((c - vwap) / scale); }
			else V.pos[i] = 0;
			// acceptance / rejection (closed bars drive the run counter; forming bar reads it)
			int run = V.acceptRun;
			float acc = 0;
			if (vwap > 0)
			{
				const int side = c > vwap ? 1 : (c < vwap ? -1 : 0);
				if (side > 0) run = run > 0 ? run + 1 : 1; else if (side < 0) run = run < 0 ? run - 1 : -1;
				if (run >= P.acceptCloses) acc = 1.0f; else if (run <= -P.acceptCloses) acc = -1.0f;
				else if (sc.Low[i] < vwap && sc.High[i] > vwap)
				{
					// traded through and closed back on the prior side = rejection
					const float prevC = i > 0 ? sc.Close[i - 1] : c;
					if (c > vwap && prevC > vwap) acc = 0.5f; else if (c < vwap && prevC < vwap) acc = -0.5f;
				}
				if (closed) V.acceptRun = run;
			}
			V.accept[i] = acc;
		}
		V.Stamp(sc);
	}

	// Phase stubs for engines not yet implemented: size arrays and mark computed so downstream code is safe.
	// ==== 8  Order Flow engine ==================================================
	namespace flow_detail
	{
		// Mean / std of the last L committed values using prefix sums (pre1/pre2 valid through index 'upto').
		inline bool WindowStats(const std::vector<double>& pre1, const std::vector<double>& pre2, int upto, int L, double& mean, double& sd)
		{
			if (upto < 0 || L < 2) return false;
			const int lo = upto - L;            // window = (lo, upto]
			const double s1 = pre1[upto] - (lo >= 0 ? pre1[lo] : 0.0);
			const double s2 = pre2[upto] - (lo >= 0 ? pre2[lo] : 0.0);
			const int cnt = upto - Max(lo, -1);
			if (cnt < 2) return false;
			mean = s1 / cnt;
			const double var = s2 / cnt - mean * mean;
			sd = var > 0 ? sqrt(var) : 0.0;
			return true;
		}

		inline double Percentile(std::vector<double> v, double pct)
		{
			if (v.empty()) return 0;
			size_t k = static_cast<size_t>(Clamp(pct, 0.0, 100.0) / 100.0 * (v.size() - 1));
			std::nth_element(v.begin(), v.begin() + k, v.end());
			return v[k];
		}

		inline float Decayed(int dir, int eventIdx, int i, int decayBars)
		{
			if (dir == 0 || eventIdx < 0 || i < eventIdx) return 0.0f;
			return static_cast<float>(dir) * static_cast<float>(exp(-static_cast<double>(i - eventIdx) / Max(1, decayBars)));
		}

		inline void SetEvent(FlowState& F, int idx, float price, const char* text)
		{
			if (idx >= F.lastEventIdx) { F.lastEventIdx = idx; F.lastEventPrice = price; strncpy_s(F.lastEventText, sizeof(F.lastEventText), text, _TRUNCATE); }
		}

		inline void PushMarker(FlowState& F, int idx, int dir, int kind, float price)
		{
			FlowState::Marker m; m.idx = idx; m.dir = dir; m.kind = kind; m.price = price; m.lineNumber = 0;
			F.markers.push_back(m);
			while (F.markers.size() > 80) { if (F.markers[0].lineNumber) F.deadLines.push_back(F.markers[0].lineNumber); F.markers.erase(F.markers.begin()); }
		}
	}

	void EnsureFlow(SCStudyInterfaceRef sc, ChartState& S)
	{
		using namespace flow_detail;
		EnsureVwap(sc, S);
		const int n = sc.ArraySize; if (n <= 0) return;
		FlowState& F = S.flow; const FlowParams& P = S.params.flow; const BaseState& B = S.base; const AuctionState& A = S.auction;
		Fit(F.delta, n); Fit(F.deltaPct, n); Fit(F.cvd, n); Fit(F.cvdZ, n); Fit(F.volZ, n);
		Fit(F.fAbsorb, n); Fit(F.fExhaust, n); Fit(F.fImb, n); Fit(F.fTrapped, n); Fit(F.fCvdDiv, n); Fit(F.fLarge, n);
		Fit(F.absorbMark, n); Fit(F.exhaustMark, n); Fit(F.imbMark, n); Fit(F.trapMark, n); Fit(F.divMark, n);
		Fit(F.volPre1, n); Fit(F.volPre2, n); Fit(F.dzPre1, n); Fit(F.dzPre2, n); Fit(F.cvdDz, n);
		if (F.UpToDate(sc)) return;
		int from = F.computedThrough + 1; if (from < 0) from = 0; if (from > n - 1) from = n - 1;
		const int L = Max(5, P.volZLength);
		const int k = Max(1, P.cvdSlopeBars);
		const float tick = S.tickSize;
		const bool haveVap = (sc.VolumeAtPriceForBars != nullptr);

		// ---- live large trades from Time & Sales (real time only; bounded buffer) ----
		{
			c_SCTimeAndSalesArray TS;
			sc.GetTimeAndSales(TS);
			const int sz = TS.Size();
			S.warn.noTS = (sz == 0);
			if (sz > 0)
			{
				TS.ValidateAndCorrectPriorSequenceNumber(F.lastTsSequence);
				int startIdx = (F.lastTsSequence == 0) ? Max(0, sz - 500) : static_cast<int>(TS.GetRecordIndexAtGreaterThanSequenceNumber(F.lastTsSequence));
				if (startIdx < 0) startIdx = 0;
				const double volMult = sc.MultiplierFromVolumeValueFormat();
				for (int t = startIdx; t < sz; ++t)
				{
					const s_TimeAndSales& r = TS[t];
					if (r.Sequence <= F.lastTsSequence && F.lastTsSequence != 0) continue;
					F.lastTsSequence = r.Sequence;
					if (r.Type != SC_TS_BID && r.Type != SC_TS_ASK) continue;
					const double size = r.GetVolume() * volMult;
					if (size <= 0) continue;
					F.tradeSizes.push_back(size);
					if (static_cast<int>(F.tradeSizes.size()) > Max(100, P.largeLookback)) F.tradeSizes.erase(F.tradeSizes.begin(), F.tradeSizes.begin() + (F.tradeSizes.size() - P.largeLookback));
					if (++F.liveSampleCount % 100 == 1 || F.liveThreshold <= 0) F.liveThreshold = Percentile(F.tradeSizes, P.largePercentile);
					if (static_cast<int>(F.tradeSizes.size()) < 50 || size < F.liveThreshold || size < P.largeMinSize) continue;
					// locate the bar: records are UTC; convert to the chart's time zone
					SCDateTime dt = r.DateTime;
					if (sc.ConvertDateTimeUTCToChartTimeZone != nullptr) dt = sc.ConvertDateTimeUTCToChartTimeZone(dt);
					int bi = n - 1;
					if (dt < sc.BaseDateTimeIn[n - 1]) { bi = sc.GetContainingIndexForSCDateTime(sc.ChartNumber, dt); if (bi < 0 || bi >= n || bi < n - 50) continue; }
					const float price = static_cast<float>(r.GetPrice() * sc.RealTimePriceMultiplier);
					const int dir = (r.Type == SC_TS_ASK) ? 1 : -1;
					bool merged = false;
					for (int q = static_cast<int>(F.bubbles.size()) - 1; q >= 0 && q >= static_cast<int>(F.bubbles.size()) - 20; --q)
					{
						Bubble& bq = F.bubbles[q];
						if (bq.idx == bi && bq.live && bq.dir == dir && fabs(bq.price - price) < 0.5f * tick) { bq.size += size; merged = true; break; }
					}
					if (!merged) { Bubble bb; bb.idx = bi; bb.price = price; bb.size = size; bb.dir = dir; bb.live = true; bb.lineNumber = 0; F.bubbles.push_back(bb); }
				}
			}
		}

		for (int i = from; i < n; ++i)
		{
			const bool closed = (i <= n - 2);
			const float atr = AtrAt(S, i);
			const float c = sc.Close[i], h = sc.High[i], l = sc.Low[i], o = sc.Open[i];
			const double vol = sc.Volume[i];
			const double ask = sc.AskVolume[i], bid = sc.BidVolume[i];
			const float range = h - l;

			// ---- delta / CVD ----
			const double delta = ask - bid;
			F.delta[i] = static_cast<float>(delta);
			F.deltaPct[i] = vol > 0 ? static_cast<float>(delta / vol) : 0.0f;
			int key = 0;
			if (P.cvdReset == 0) key = B.isRth[i] ? B.rthSession[i] : -1;
			else if (P.cvdReset == 1) key = B.tradingDay[i];
			else key = 1;
			if (key != F.cvdSession) { F.cvdCommitted = 0; F.cvdSession = key; }
			const double cvd = F.cvdCommitted + delta;
			F.cvd[i] = static_cast<float>(cvd);
			if (closed) F.cvdCommitted = cvd;

			// ---- rolling z-scores via prefix sums over closed bars ----
			const double dz = (i - k >= 0) ? static_cast<double>(F.cvd[i]) - F.cvd[i - k] : 0.0;
			F.cvdDz[i] = static_cast<float>(dz);
			{
				const int upto = i - 1;
				double m = 0, sd = 0;
				if (WindowStats(F.volPre1, F.volPre2, upto, L, m, sd) && sd > 0) F.volZ[i] = static_cast<float>(Clamp((vol - m) / sd, -6.0, 6.0)); else F.volZ[i] = 0;
				if (WindowStats(F.dzPre1, F.dzPre2, upto, L, m, sd) && sd > 0) F.cvdZ[i] = static_cast<float>(Clamp((dz - m) / sd, -6.0, 6.0)); else F.cvdZ[i] = 0;
			}
			if (closed)
			{
				F.volPre1[i] = (i > 0 ? F.volPre1[i - 1] : 0.0) + vol; F.volPre2[i] = (i > 0 ? F.volPre2[i - 1] : 0.0) + vol * vol;
				F.dzPre1[i] = (i > 0 ? F.dzPre1[i - 1] : 0.0) + dz; F.dzPre2[i] = (i > 0 ? F.dzPre2[i - 1] : 0.0) + dz * dz;
			}

			// ---- events (closed bars only) ----
			if (closed)
			{
				F.absorbMark[i] = F.exhaustMark[i] = F.imbMark[i] = F.trapMark[i] = F.divMark[i] = 0;

				// absorption: heavy aggressive volume, little progress, close back inside
				if (F.volZ[i] >= P.absorbVolZ && atr > 0 && range <= P.absorbMaxRangeAtr * atr && range > 0 && i >= 3)
				{
					const float prevLow = Min(sc.Low[i - 1], Min(sc.Low[i - 2], sc.Low[i - 3]));
					const float prevHigh = Max(sc.High[i - 1], Max(sc.High[i - 2], sc.High[i - 3]));
					if (F.deltaPct[i] <= -0.10f && (c - l) >= 0.4f * range && l <= prevLow)
					{
						Zone z; z.bornIdx = i; z.bottom = l; z.top = l + P.absorbZoneFrac * range; z.dir = 1; z.kind = LVL_ABSORB; z.active = true;
						F.absorbZones.push_back(z); F.absorbMark[i] = 1; F.lastAbsIdx = i; F.lastAbsDir = 1; SetEvent(F, i, l, "Absorption (buyers)");
					}
					else if (F.deltaPct[i] >= 0.10f && (h - c) >= 0.4f * range && h >= prevHigh)
					{
						Zone z; z.bornIdx = i; z.top = h; z.bottom = h - P.absorbZoneFrac * range; z.dir = -1; z.kind = LVL_ABSORB; z.active = true;
						F.absorbZones.push_back(z); F.absorbMark[i] = -1; F.lastAbsIdx = i; F.lastAbsDir = -1; SetEvent(F, i, h, "Absorption (sellers)");
					}
					while (static_cast<int>(F.absorbZones.size()) > Max(5, P.maxActiveZones) * 2) { if (F.absorbZones[0].lineNumber) F.deadLines.push_back(F.absorbZones[0].lineNumber); F.absorbZones.erase(F.absorbZones.begin()); }
				}

				// VAP-based: exhaustion, stacked imbalances, historical large-trade clusters
				if (haveVap && static_cast<int>(sc.VolumeAtPriceForBars->GetNumberOfBars()) > i)
				{
					const int cnt = sc.VolumeAtPriceForBars->GetSizeAtBarIndex(i);
					if (cnt >= 2)
					{
						std::vector<const s_VolumeAtPriceV2*> lv; lv.reserve(cnt);
						for (int q = 0; q < cnt; ++q) { const s_VolumeAtPriceV2* p = nullptr; if (sc.VolumeAtPriceForBars->GetVAPElementAtIndex(i, q, &p) && p) lv.push_back(p); }
						const int m = static_cast<int>(lv.size());
						if (m >= 2)
						{
							// exhaustion: thin volume at the extreme after a run
							double maxLevelVol = 0; for (int q = 0; q < m; ++q) maxLevelVol = Max(maxLevelVol, lv[q]->Volume);
							const int run = Max(1, P.exhaustRunBars);
							if (i - run >= 0 && maxLevelVol > 0)
							{
								bool upRun = c > sc.Close[i - run], downRun = c < sc.Close[i - run];
								for (int q = 1; q <= run && q <= i; ++q) { if (sc.High[i - q + 1] < sc.High[i - q]) upRun = false; if (sc.Low[i - q + 1] > sc.Low[i - q]) downRun = false; }
								const double thin = P.exhaustExtremePct / 100.0 * maxLevelVol;
								if (upRun && lv[m - 1]->Volume <= thin && (m < 3 || lv[m - 2]->Volume <= thin * 1.5)) { F.exhaustMark[i] = -1; F.lastExhIdx = i; F.lastExhDir = -1; SetEvent(F, i, h, "Exhaustion top"); PushMarker(F, i, -1, 3, h); }
								else if (downRun && lv[0]->Volume <= thin && (m < 3 || lv[1]->Volume <= thin * 1.5)) { F.exhaustMark[i] = 1; F.lastExhIdx = i; F.lastExhDir = 1; SetEvent(F, i, l, "Exhaustion bottom"); PushMarker(F, i, 1, 3, l); }
							}

							// stacked diagonal imbalances (Sierra Numbers Bars definition)
							const double ratio = P.imbRatioPct / 100.0; const double minV = P.imbMinVolume;
							int buyRun = 0, sellRun = 0; int buyStart = -1, sellStart = -1;
							for (int q = 0; q < m; ++q)
							{
								const bool adjBelow = q > 0 && lv[q]->PriceInTicks - lv[q - 1]->PriceInTicks == 1;
								const bool adjAbove = q + 1 < m && lv[q + 1]->PriceInTicks - lv[q]->PriceInTicks == 1;
								const bool buyImb = adjBelow && lv[q]->AskVolume >= minV && lv[q - 1]->BidVolume >= minV && lv[q]->AskVolume >= ratio * lv[q - 1]->BidVolume;
								const bool sellImb = adjAbove && lv[q]->BidVolume >= minV && lv[q + 1]->AskVolume >= minV && lv[q]->BidVolume >= ratio * lv[q + 1]->AskVolume;
								if (buyImb) { if (buyRun == 0) buyStart = q; ++buyRun; } else buyRun = 0;
								if (sellImb) { if (sellRun == 0) sellStart = q; ++sellRun; } else sellRun = 0;
								if (buyRun >= P.imbStackLevels && (q + 1 >= m || !(q + 1 < m && lv[q + 1]->PriceInTicks - lv[q]->PriceInTicks == 1 && lv[q + 1]->AskVolume >= minV && lv[q]->BidVolume >= minV && lv[q + 1]->AskVolume >= ratio * lv[q]->BidVolume)))
								{
									Zone z; z.bornIdx = i; z.bottom = (lv[buyStart]->PriceInTicks - 0.5f) * tick; z.top = (lv[q]->PriceInTicks + 0.5f) * tick; z.dir = 1; z.kind = LVL_IMB; z.active = true;
									F.imbZones.push_back(z); F.imbMark[i] = static_cast<signed char>(F.imbMark[i] == -1 ? 2 : 1); F.lastImbIdx = i; F.lastImbDir = 1; SetEvent(F, i, z.bottom, "Stacked buy imbalance");
									buyRun = 0;
								}
								if (sellRun >= P.imbStackLevels && (q + 1 >= m || !(q + 2 < m && lv[q + 2]->PriceInTicks - lv[q + 1]->PriceInTicks == 1 && lv[q + 1]->BidVolume >= minV && lv[q + 2]->AskVolume >= minV && lv[q + 1]->BidVolume >= ratio * lv[q + 2]->AskVolume)))
								{
									Zone z; z.bornIdx = i; z.bottom = (lv[sellStart]->PriceInTicks - 0.5f) * tick; z.top = (lv[q]->PriceInTicks + 0.5f) * tick; z.dir = -1; z.kind = LVL_IMB; z.active = true;
									F.imbZones.push_back(z); F.imbMark[i] = static_cast<signed char>(F.imbMark[i] == 1 ? 2 : -1); F.lastImbIdx = i; F.lastImbDir = -1; SetEvent(F, i, z.top, "Stacked sell imbalance");
									sellRun = 0;
								}
							}
							while (static_cast<int>(F.imbZones.size()) > Max(5, P.maxActiveZones) * 2) { if (F.imbZones[0].lineNumber) F.deadLines.push_back(F.imbZones[0].lineNumber); F.imbZones.erase(F.imbZones.begin()); }

							// historical large-trade clusters (skip bars that already got live bubbles)
							bool hasLive = false;
							for (int q = static_cast<int>(F.bubbles.size()) - 1; q >= 0 && F.bubbles[q].idx >= i; --q) if (F.bubbles[q].idx == i && F.bubbles[q].live) { hasLive = true; break; }
							for (int q = 0; q < m; ++q)
							{
								if (lv[q]->NumberOfTrades == 0) continue;
								const double avg = lv[q]->Volume / lv[q]->NumberOfTrades;
								F.levelAvgSizes.push_back(avg);
								if (static_cast<int>(F.levelAvgSizes.size()) > 4000) F.levelAvgSizes.erase(F.levelAvgSizes.begin(), F.levelAvgSizes.begin() + 1000);
							}
							if (++F.ltSampleCount % 25 == 1 || F.ltThreshold <= 0) F.ltThreshold = Percentile(F.levelAvgSizes, P.largePercentile);
							if (!hasLive && static_cast<int>(F.levelAvgSizes.size()) >= 200)
							{
								for (int q = 0; q < m; ++q)
								{
									if (lv[q]->NumberOfTrades == 0) continue;
									const double avg = lv[q]->Volume / lv[q]->NumberOfTrades;
									if (avg < F.ltThreshold || avg < P.largeMinSize) continue;
									Bubble bb; bb.idx = i; bb.price = lv[q]->PriceInTicks * tick; bb.size = lv[q]->Volume; bb.dir = lv[q]->AskVolume >= lv[q]->BidVolume ? 1 : -1; bb.live = false; bb.lineNumber = 0;
									F.bubbles.push_back(bb);
								}
							}
						}
					}
				}
				while (static_cast<int>(F.bubbles.size()) > Max(10, P.maxBubbles) + 50) { if (F.bubbles[0].lineNumber) F.deadLines.push_back(F.bubbles[0].lineNumber); F.bubbles.erase(F.bubbles.begin()); }

				// zones die when traded through
				for (size_t z = 0; z < F.absorbZones.size(); ++z) { Zone& Z = F.absorbZones[z]; if (Z.active && i > Z.bornIdx && ((Z.dir > 0 && c < Z.bottom) || (Z.dir < 0 && c > Z.top))) { Z.active = false; Z.deadIdx = i; } }
				for (size_t z = 0; z < F.imbZones.size(); ++z) { Zone& Z = F.imbZones[z]; if (Z.active && i > Z.bornIdx && ((Z.dir > 0 && c < Z.bottom) || (Z.dir < 0 && c > Z.top))) { Z.active = false; Z.deadIdx = i; } }

				// trapped traders: strong-delta breakout fully reversed within K bars
				{
					const int LB = Max(3, P.trapLookback);
					for (size_t q = 0; q < F.pendingBreakouts.size(); )
					{
						FlowState::Breakout& bo = F.pendingBreakouts[q];
						bool done = false;
						if (i - bo.idx > P.trapReversalBars) done = true;
						else if (bo.dir > 0 && c < bo.extreme) { F.trapMark[i] = -1; F.lastTrapIdx = i; F.lastTrapDir = -1; SetEvent(F, i, bo.extreme, "Trapped longs"); PushMarker(F, i, -1, 2, h); done = true; }
						else if (bo.dir < 0 && c > bo.extreme) { F.trapMark[i] = 1; F.lastTrapIdx = i; F.lastTrapDir = 1; SetEvent(F, i, bo.extreme, "Trapped shorts"); PushMarker(F, i, 1, 2, l); done = true; }
						if (done) F.pendingBreakouts.erase(F.pendingBreakouts.begin() + q); else ++q;
					}
					if (i - LB >= 0)
					{
						float hh = -FLT_MAX, ll = FLT_MAX;
						for (int q = i - LB; q < i; ++q) { hh = Max(hh, sc.High[q]); ll = Min(ll, sc.Low[q]); }
						const float minD = P.trapMinDeltaPct / 100.0f;
						if (c > hh && F.deltaPct[i] >= minD && F.volZ[i] >= 0.5f) { FlowState::Breakout bo; bo.idx = i; bo.dir = 1; bo.extreme = l; F.pendingBreakouts.push_back(bo); }
						else if (c < ll && F.deltaPct[i] <= -minD && F.volZ[i] >= 0.5f) { FlowState::Breakout bo; bo.idx = i; bo.dir = -1; bo.extreme = h; F.pendingBreakouts.push_back(bo); }
					}
				}

				// CVD divergence at a freshly confirmed swing (confirmation bar = i)
				if (!A.swings.empty() && A.swings.back().confirmIdx == i)
				{
					const Swing& ns = A.swings.back();
					const int prevIdx = ns.high ? A.prevSwingHigh : A.prevSwingLow;
					if (prevIdx >= 0 && prevIdx < static_cast<int>(A.swings.size()) - 1)
					{
						const Swing& ps = A.swings[prevIdx];
						if (fabs(ns.price - ps.price) >= P.divMinAtr * atr && ns.idx < n && ps.idx < n)
						{
							const float cvdNew = F.cvd[ns.idx], cvdOld = F.cvd[ps.idx];
							if (ns.high && ns.price > ps.price && cvdNew <= cvdOld) { F.divMark[i] = -1; F.lastDivIdx = i; F.lastDivDir = -1; F.lastDivPrice = ns.price; PushMarker(F, ns.idx, -1, 1, ns.price); }
							else if (!ns.high && ns.price < ps.price && cvdNew >= cvdOld) { F.divMark[i] = 1; F.lastDivIdx = i; F.lastDivDir = 1; F.lastDivPrice = ns.price; PushMarker(F, ns.idx, 1, 1, ns.price); }
						}
					}
				}
			}

			// ---- features ----
			const int decay = Max(1, P.eventDecayBars);
			F.fAbsorb[i] = Decayed(F.lastAbsDir, F.lastAbsIdx, i, decay);
			F.fExhaust[i] = Decayed(F.lastExhDir, F.lastExhIdx, i, decay);
			F.fImb[i] = Decayed(F.lastImbDir, F.lastImbIdx, i, decay);
			F.fTrapped[i] = Decayed(F.lastTrapDir, F.lastTrapIdx, i, decay);
			F.fCvdDiv[i] = Decayed(F.lastDivDir, F.lastDivIdx, i, decay * 2);
			{
				double buy = 0, sell = 0;
				for (int q = static_cast<int>(F.bubbles.size()) - 1; q >= 0; --q)
				{
					const Bubble& bb = F.bubbles[q];
					if (bb.idx > i - 1) continue;            // closed bars only
					if (bb.idx < i - 10) break;
					if (bb.dir > 0) buy += bb.size; else sell += bb.size;
				}
				F.fLarge[i] = (buy + sell) > 0 ? static_cast<float>((buy - sell) / (buy + sell)) : 0.0f;
			}
		}
		F.Stamp(sc);
	}

	// ==== 9  Regime + MTF engine ================================================
	namespace regime_detail
	{
		static const int kTfMinutes[4] = { 1, 5, 15, 60 };
		static const float kTfWeight[4] = { 0.10f, 0.20f, 0.30f, 0.40f };

		// Bucket key for a bar time at timeframe T minutes (date * 1440 + minute-of-day / T).
		inline int BucketKey(const SCDateTime& dt, int tfMin) { return dt.GetDate() * 1440 + (dt.GetTimeInSeconds() / 60) / tfMin; }

		// Commit a completed timeframe bar: EMA, fractal pivots, structure state.
		void CommitTfBar(RegimeState::Tf& tf, const RegimeParams& P)
		{
			RegimeState::TfBar& b = tf.cur;
			if (b.count == 0) return;
			tf.bars.push_back(b);
			if (tf.bars.size() > 2000) tf.bars.erase(tf.bars.begin(), tf.bars.begin() + 500);
			const float a = 2.0f / (Max(2, P.mtfEmaLength) + 1.0f);
			tf.ema = (tf.emaCount == 0) ? b.c : tf.ema + a * (b.c - tf.ema);
			tf.emaHist.push_back(tf.ema);
			if (tf.emaHist.size() > 2000) tf.emaHist.erase(tf.emaHist.begin(), tf.emaHist.begin() + 500);
			++tf.emaCount;
			// fractal pivot confirmed at bars.size()-1-s
			const int s = Max(1, P.mtfSwing);
			const int m = static_cast<int>(tf.bars.size());
			const int p = m - 1 - s;
			if (p - s >= 0)
			{
				bool ph = true, pl = true;
				for (int k = 1; k <= s; ++k)
				{
					if (tf.bars[p].h <= tf.bars[p - k].h || tf.bars[p].h <= tf.bars[p + k].h) ph = false;
					if (tf.bars[p].l >= tf.bars[p - k].l || tf.bars[p].l >= tf.bars[p + k].l) pl = false;
				}
				if (ph) { tf.prevHigh = tf.lastHigh; tf.lastHigh = tf.bars[p].h; }
				if (pl) { tf.prevLow = tf.lastLow; tf.lastLow = tf.bars[p].l; }
			}
			b = RegimeState::TfBar();
		}
	}

	void EnsureRegime(SCStudyInterfaceRef sc, ChartState& S)
	{
		using namespace regime_detail;
		EnsureFlow(sc, S);
		const int n = sc.ArraySize; if (n <= 0) return;
		RegimeState& R = S.regime; const RegimeParams& P = S.params.regime; const BaseState& B = S.base; const AuctionState& A = S.auction; const VwapState& V = S.vwap;
		Fit(R.er, n); Fit(R.atrRatio, n); Fit(R.regimeTrend, n); Fit(R.mtfBias, n); Fit(R.regime, n);
		Fit(R.mtf1, n); Fit(R.mtf5, n); Fit(R.mtf15, n); Fit(R.mtf60, n); Fit(R.atrFast, n); Fit(R.atrSlow, n);
		Fit(R.absDcPre, n); Fit(R.insidePre, n); Fit(R.trendiness, n); Fit(R.dirScore, n);
		if (R.UpToDate(sc)) return;
		int from = R.computedThrough + 1; if (from < 0) from = 0; if (from > n - 1) from = n - 1;
		const int L = Max(2, P.erLength);
		const int fastL = Max(2, P.atrFast), slowL = Max(fastL + 1, P.atrSlow);
		const int insideN = Max(5, P.insideValueBars);
		const int secPerBar = sc.SecondsPerBar;

		for (int i = from; i < n; ++i)
		{
			const bool closed = (i <= n - 2);
			const float c = sc.Close[i];
			const float atr = AtrAt(S, i);

			// ---- efficiency ratio ----
			const double adc = (i > 0) ? fabs(static_cast<double>(c) - sc.Close[i - 1]) : 0.0;
			const double preBase = (i > 0) ? R.absDcPre[i - 1] : 0.0;
			const double preLo = (i - L - 1 >= 0) ? R.absDcPre[i - L - 1] : 0.0;
			double path = (preBase - preLo) + adc;         // sum of |dc| over bars i-L+1..i
			if (i - L >= 0 && path > 0) R.er[i] = static_cast<float>(Clamp(fabs(static_cast<double>(c) - sc.Close[i - L]) / path, 0.0, 1.0)); else R.er[i] = 0;
			if (closed) R.absDcPre[i] = preBase + adc;

			// ---- ATR expansion ----
			const float tr = S.base.tr[i];
			if (i == 0) { R.atrFast[i] = tr; R.atrSlow[i] = tr; }
			else
			{
				R.atrFast[i] = (i < fastL) ? (R.atrFast[i - 1] * i + tr) / (i + 1) : (R.atrFast[i - 1] * (fastL - 1) + tr) / fastL;
				R.atrSlow[i] = (i < slowL) ? (R.atrSlow[i - 1] * i + tr) / (i + 1) : (R.atrSlow[i - 1] * (slowL - 1) + tr) / slowL;
			}
			R.atrRatio[i] = R.atrSlow[i] > 0 ? R.atrFast[i] / R.atrSlow[i] : 1.0f;

			// ---- inside-value fraction ----
			const double inside = (A.vah[i] > A.val[i] && c <= A.vah[i] && c >= A.val[i]) ? 1.0 : 0.0;
			const double inBase = (i > 0) ? R.insidePre[i - 1] : 0.0;
			const double inLo = (i - insideN - 1 >= 0) ? R.insidePre[i - insideN - 1] : 0.0;
			const int cnt = Min(insideN, i + 1);
			const float insideFrac = cnt > 0 ? static_cast<float>(((inBase - inLo) + inside) / cnt) : 0.0f;
			if (closed) R.insidePre[i] = inBase + inside;

			// ---- IB relative width (daily context) ----
			float ibRel = 1.0f;
			if (A.ibDone[i] && A.ibHigh[i] > A.ibLow[i] && !A.ibRangeHistory.empty())
			{
				double sum = 0; int m = 0;
				for (int k = static_cast<int>(A.ibRangeHistory.size()) - 1; k >= 0 && m < P.ibAvgDays; --k, ++m) sum += A.ibRangeHistory[k];
				if (m > 0 && sum > 0) ibRel = static_cast<float>((A.ibHigh[i] - A.ibLow[i]) / (sum / m));
			}

			// ---- scores ----
			const float vs = V.slope[i], st = A.structTrend[i], vm = A.valueMig[i];
			const float side = (V.vwap[i] > 0) ? Sign(c - V.vwap[i]) : 0.0f;
			const float erN = Clamp(R.er[i] / Max(0.05f, P.erTrend), 0.0f, 1.0f);
			float trendiness = 0.35f * erN + 0.25f * static_cast<float>(fabs(vs)) + 0.20f * static_cast<float>(fabs(st)) + 0.20f * (1.0f - insideFrac);
			if (ibRel > 1.2f) trendiness += 0.05f; else if (ibRel < 0.8f) trendiness -= 0.05f;
			trendiness = Clamp(trendiness, 0.0f, 1.0f);
			const float dir = Clamp1(0.4 * vs + 0.3 * st + 0.2 * side + 0.1 * vm);
			R.trendiness[i] = trendiness; R.dirScore[i] = dir;
			const bool chop = R.atrRatio[i] >= P.chopExpansion && R.er[i] < P.erBalance;
			int cand = R.current;
			if (chop) cand = RG_CHOP;
			else if (R.er[i] >= P.erTrend && fabs(dir) >= 0.3f && trendiness >= 0.45f) cand = dir > 0 ? RG_TREND_UP : RG_TREND_DOWN;
			else if (R.er[i] <= P.erBalance || insideFrac >= 0.6f || trendiness < 0.3f) cand = RG_BALANCE;
			else if (R.current == RG_NONE) cand = RG_BALANCE;
			if (closed)
			{
				if (cand == R.current) { R.candidate = cand; R.candidateCount = 0; }
				else if (cand == R.candidate) { if (++R.candidateCount >= Max(1, P.hysteresisBars) || R.current == RG_NONE) { R.current = cand; R.candidateCount = 0; } }
				else { R.candidate = cand; R.candidateCount = 1; if (R.current == RG_NONE) R.current = cand; }
			}
			const int reg = (R.current == RG_NONE) ? cand : R.current;
			R.regime[i] = static_cast<signed char>(reg);
			float rt = 0;
			if (reg == RG_TREND_UP || reg == RG_TREND_DOWN) rt = Clamp1(dir * (0.5 + 0.5 * trendiness));
			else if (reg == RG_BALANCE) rt = 0.3f * dir;
			else rt = 0.0f;
			R.regimeTrend[i] = rt;

			// ---- MTF cells (completed timeframe bars + current close vs VWAP) ----
			float bias = 0, wsum = 0;
			signed char cells[4] = { 0, 0, 0, 0 };
			for (int t = 0; t < 4; ++t)
			{
				RegimeState::Tf& tf = R.tf[t];
				const int tfMin = kTfMinutes[t];
				tf.available = (secPerBar <= 0) || (secPerBar <= tfMin * 60);
				if (!tf.available) { cells[t] = 0; continue; }
				const int key = BucketKey(sc.BaseDateTimeIn[i], tfMin);
				if (key != tf.curKey)
				{
					// the previous timeframe bar is complete once a bar of a new bucket exists (closed or forming)
					if (tf.curKey != INT_MIN && tf.cur.count > 0 && tf.committedKey != tf.curKey) { tf.committedKey = tf.curKey; CommitTfBar(tf, P); }
					if (closed) { tf.curKey = key; tf.cur = RegimeState::TfBar(); }
				}
				if (closed && key == tf.curKey)
				{
					RegimeState::TfBar& b = tf.cur;
					if (b.count == 0) { b.o = sc.Open[i]; b.h = sc.High[i]; b.l = sc.Low[i]; } else { b.h = Max(b.h, sc.High[i]); b.l = Min(b.l, sc.Low[i]); }
					b.c = c; b.startIdx = b.count == 0 ? i : b.startIdx; ++b.count;
				}
				// structure from confirmed TF pivots
				float sTf = 0;
				if (tf.lastHigh > 0 && tf.prevHigh > 0 && tf.lastLow > 0 && tf.prevLow > 0)
				{
					const bool hh = tf.lastHigh > tf.prevHigh, hl = tf.lastLow > tf.prevLow;
					sTf = (hh && hl) ? 1.0f : ((!hh && !hl) ? -1.0f : 0.0f);
				}
				float eSl = 0;
				const int eh = static_cast<int>(tf.emaHist.size());
				if (eh >= 4 && atr > 0) eSl = Clamp1((tf.emaHist[eh - 1] - tf.emaHist[eh - 4]) / (atr * sqrt(static_cast<double>(tfMin))));
				const float sum = 0.4f * sTf + 0.3f * side + 0.3f * eSl;
				cells[t] = static_cast<signed char>(sum >= 0.3f ? 1 : (sum <= -0.3f ? -1 : 0));
				bias += kTfWeight[t] * sum; wsum += kTfWeight[t];
			}
			R.mtf1[i] = cells[0]; R.mtf5[i] = cells[1]; R.mtf15[i] = cells[2]; R.mtf60[i] = cells[3];
			R.mtfBias[i] = wsum > 0 ? Clamp1(bias / wsum) : 0.0f;
		}
		R.Stamp(sc);
	}

	void EnsureInter(SCStudyInterfaceRef sc, ChartState& S)
	{
		EnsureRegime(sc, S); const int n = sc.ArraySize; InterState& I = S.inter;
		Fit(I.rsYM, n); Fit(I.rsES, n); Fit(I.rsRTY, n); Fit(I.rsIndex, n); Fit(I.smt, n); Fit(I.tickCum, n); Fit(I.tickExt, n);
		Fit(I.tickDiv, n); Fit(I.megaCap, n); Fit(I.composite, n); Fit(I.smtMark, n);
		I.computedThrough = n - 2; I.lastArraySize = n;
	}
	void EnsureDcs(SCStudyInterfaceRef sc, ChartState& S)
	{
		EnsureInter(sc, S); const int n = sc.ArraySize; DcsState& D = S.dcs;
		Fit(D.feat, n * F_COUNT); Fit(D.dcs, n); Fit(D.dcsSmooth, n); Fit(D.barState, n); Fit(D.signalType, n); Fit(D.signalDir, n);
		D.computedThrough = n - 2; D.lastArraySize = n;
	}
	void EnsureVal(SCStudyInterfaceRef sc, ChartState& S)
	{
		EnsureDcs(sc, S); const int n = sc.ArraySize; Fit(S.val.cumR, n);
		S.val.computedThrough = n - 2; S.val.lastArraySize = n;
	}
	void EnsureLog(SCStudyInterfaceRef sc, ChartState& S)
	{
		EnsureVal(sc, S); S.log.computedThrough = sc.ArraySize - 2; S.log.lastArraySize = sc.ArraySize;
	}

	// ==== 14 HUD snapshot + GDI =================================================

	void CheckWarnings(SCStudyInterfaceRef sc, ChartState& S)
	{
		Warnings& W = S.warn;
		W.storageNotTick = (sc.IntradayDataStorageTimeUnit != kStorageUnitTick);
		W.tzNotNY = false;
		if (sc.GetChartTimeZone != nullptr)
		{
			SCString tz = sc.GetChartTimeZone(sc.ChartNumber);
			const char* s = tz.GetChars();
			std::string t = s ? s : "";
			for (size_t k = 0; k < t.size(); ++k) t[k] = static_cast<char>(tolower(static_cast<unsigned char>(t[k])));
			W.tzNotNY = !(t.find("new_york") != std::string::npos || t.find("new york") != std::string::npos || t.find("eastern") != std::string::npos);
		}
		W.vapOff = (sc.MaintainVolumeAtPriceData == 0);
		W.noDepth = (sc.GetBidMarketDepthNumberOfLevels != nullptr) ? (sc.GetBidMarketDepthNumberOfLevels() <= 0) : true;
		W.text[0] = 0;
		if (W.storageNotTick) strcat_s(W.text, sizeof(W.text), "Storage unit must be 1 TICK (Global Settings>>Data/Trade Service Settings)\n");
		if (W.tzNotNY) strcat_s(W.text, sizeof(W.text), "Chart time zone must be New York (Chart>>Chart Settings)\n");
		if (W.vapOff) strcat_s(W.text, sizeof(W.text), "Volume at Price data off (reload chart after adding NQ Edge studies)\n");
		if (W.interMissing[0]) { strcat_s(W.text, sizeof(W.text), "Missing intermarket: "); strcat_s(W.text, sizeof(W.text), W.interMissing); strcat_s(W.text, sizeof(W.text), "\n"); }
		if (W.noDepth) strcat_s(W.text, sizeof(W.text), "No market depth (optional; depth features disabled)\n");
	}

	void BuildHudSnapshot(SCStudyInterfaceRef sc, ChartState& S)
	{
		HudSnapshot& H = S.hud;
		const int n = sc.ArraySize;
		const int i = n - 2;   // last closed bar
		H.lastClosedIdx = i;
		if (i < 0) return;
		H.close = sc.Close[n - 1];
		H.atr = AtrAt(S, i);
		if (i < static_cast<int>(S.dcs.dcs.size()))
		{
			H.dcs = S.dcs.dcs[i]; H.dcsSmooth = S.dcs.dcsSmooth[i];
			H.dcsTrend = (i >= 5) ? S.dcs.dcs[i] - S.dcs.dcs[i - 5] : 0;
			H.barState = S.dcs.barState[i];
			H.bias = (H.dcs >= S.dcs.weights.thrSignal) ? 1 : (H.dcs <= -S.dcs.weights.thrSignal ? -1 : 0);
		}
		if (i < static_cast<int>(S.regime.regime.size()))
		{
			H.regime = S.regime.regime[i];
			H.mtf[0] = S.regime.mtf1[i]; H.mtf[1] = S.regime.mtf5[i]; H.mtf[2] = S.regime.mtf15[i]; H.mtf[3] = S.regime.mtf60[i];
		}
		if (i < static_cast<int>(S.auction.openType.size())) { H.openType = S.auction.openType[i]; H.valueMig = S.auction.valueMig[i]; }
		if (i < static_cast<int>(S.flow.cvdZ.size()))
		{
			H.cvdZ = S.flow.cvdZ[i]; H.cvdTrend = H.cvdZ > 0.5f ? 1 : (H.cvdZ < -0.5f ? -1 : 0);
			strcpy_s(H.lastEvent, sizeof(H.lastEvent), S.flow.lastEventText); H.lastEventPrice = S.flow.lastEventPrice;
		}
		H.signalsTotal = static_cast<int>(S.dcs.signals.size());
		if (i < static_cast<int>(S.vwap.vwap.size()) && S.vwap.vwap[i] > 0)
			sprintf_s(H.vwapText, sizeof(H.vwapText), "VWAP %s", sc.FormatGraphValue(S.vwap.vwap[i], sc.BaseGraphValueFormat).GetChars());
		else H.vwapText[0] = 0;
	}

	inline n_ACSIL::s_GraphicsColor GColor(uint32_t c) { n_ACSIL::s_GraphicsColor g; g.SetColorValue(c); return g; }

	struct HudPainter
	{
		SCStudyInterfaceRef sc; int x, y, w, lineH, fontPt; uint32_t textColor;
		HudPainter(SCStudyInterfaceRef s, int px, int py, int pw, int lh, int fp, uint32_t tc) : sc(s), x(px), y(py), w(pw), lineH(lh), fontPt(fp), textColor(tc) {}
		void Font(int pt, bool bold)
		{
			n_ACSIL::s_GraphicsFont f; f.m_FaceName = "Consolas"; f.m_Height = pt; f.m_Weight = bold ? FW_BOLD : FW_NORMAL;
			sc.Graphics.SetTextFont(f);
		}
		void Text(const char* s, uint32_t color, int dx = 0) { sc.Graphics.SetTextColor(GColor(color)); sc.Graphics.DrawTextAt(SCString(s), x + 8 + dx, y); }
		void Line(const char* s, uint32_t color) { Text(s, color); y += lineH; }
		int TextWidth(const char* s) { n_ACSIL::s_GraphicsSize sz; sc.Graphics.GetTextSize(SCString(s), sz); return sz.Width; }
		void Dot(int cx, int cy, int r, uint32_t color)
		{
			n_ACSIL::s_GraphicsBrush b; b.m_BrushType = n_ACSIL::s_GraphicsBrush::BRUSH_TYPE_SOLID; b.m_BrushColor.SetColorValue(color);
			n_ACSIL::s_GraphicsPen p; p.m_PenColor.SetColorValue(color); p.m_Width = 1; sc.Graphics.SetPen(p);
			sc.Graphics.FillEllipse(cx - r, cy - r, cx + r, cy + r, b);
		}
		void Box(int left, int top, int right, int bottom, uint32_t color)
		{
			n_ACSIL::s_GraphicsBrush b; b.m_BrushType = n_ACSIL::s_GraphicsBrush::BRUSH_TYPE_SOLID; b.m_BrushColor.SetColorValue(color);
			n_ACSIL::s_GraphicsRectangle r; r.Left = left; r.Top = top; r.Right = right; r.Bottom = bottom;
			sc.Graphics.FillRectangle(r, b);
		}
	};

	// HUD input indices (shared by the study function and the GDI callback)
	enum HudInput
	{
		HI_POSITION = 0, HI_PRESET, HI_FONT, HI_WIDTH, HI_OPACITY, HI_PAINT, HI_SHOW_WARN, HI_SHOW_STATS, HI_SHOW_INTER, HI_SHOW_FLOW, HI_SHOW_LEVELS,
		HI_C_BG, HI_C_TEXT, HI_C_BULL, HI_C_BEAR, HI_C_NEUTRAL, HI_C_LEVEL, HI_C_VWAP, HI_C_WARN,
		HI_C_STRONG_BULL, HI_C_WEAK_BULL, HI_C_NEUTRAL_BAR, HI_C_WEAK_BEAR, HI_C_STRONG_BEAR, HI_COUNT
	};

	void DrawHUD(HWND WindowHandle, HDC DeviceContext, SCStudyInterfaceRef sc)
	{
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		ChartState* Sp = Peek(sc.ChartNumber);
		if (Sp == nullptr) return;
		ChartState& S = *Sp;
		const HudSnapshot& H = S.hud;
		if (sc.Graphics.DrawTextAt == nullptr || sc.Graphics.FillRectangle == nullptr) return;

		const int fontPt = Max(7, sc.Input[HI_FONT].GetInt());
		const int lineH = fontPt + 6;
		const int panelW = Max(200, sc.Input[HI_WIDTH].GetInt());
		const bool full = sc.Input[HI_PRESET].GetIndex() == 1;
		const uint32_t cBg = sc.Input[HI_C_BG].GetColor(), cText = sc.Input[HI_C_TEXT].GetColor();
		const uint32_t cBull = sc.Input[HI_C_BULL].GetColor(), cBear = sc.Input[HI_C_BEAR].GetColor(), cNeu = sc.Input[HI_C_NEUTRAL].GetColor();
		const uint32_t cLevel = sc.Input[HI_C_LEVEL].GetColor(), cVwap = sc.Input[HI_C_VWAP].GetColor(), cWarn = sc.Input[HI_C_WARN].GetColor();

		// line budget
		int lines = 2;                      // bias + regime
		lines += 1;                         // mtf
		if (sc.Input[HI_SHOW_INTER].GetYesNo()) lines += 1;
		if (sc.Input[HI_SHOW_FLOW].GetYesNo()) lines += 1;
		if (sc.Input[HI_SHOW_LEVELS].GetYesNo()) lines += 1;
		lines += 1;                         // state line
		if (full && sc.Input[HI_SHOW_STATS].GetYesNo()) lines += 2;
		int warnLines = 0;
		if (sc.Input[HI_SHOW_WARN].GetYesNo() && S.warn.text[0]) { for (const char* p = S.warn.text; *p; ++p) if (*p == '\n') ++warnLines; }
		lines += warnLines;
		const int panelH = lines * lineH + lineH + 10;

		const int pos = sc.Input[HI_POSITION].GetIndex();
		const int left0 = sc.StudyRegionLeftCoordinate, top0 = sc.StudyRegionTopCoordinate;
		const int right0 = sc.StudyRegionRightCoordinate, bottom0 = sc.StudyRegionBottomCoordinate;
		int px = left0 + 10, py = top0 + 10;
		if (pos == 1 || pos == 3) px = right0 - panelW - 10;
		if (pos == 2 || pos == 3) py = bottom0 - panelH - 10;
		if (px < left0) px = left0; if (py < top0) py = top0;

		// panel background
		n_ACSIL::s_GraphicsRectangle rect; rect.Left = px; rect.Top = py; rect.Right = px + panelW; rect.Bottom = py + panelH;
		const int opacity = Clamp(sc.Input[HI_OPACITY].GetInt(), 0, 100);
		if (sc.Graphics.FillRectangleWithColorTransparent != nullptr && opacity < 100)
			sc.Graphics.FillRectangleWithColorTransparent(rect, GColor(cBg), static_cast<uint8_t>(100 - opacity));
		else
			sc.Graphics.FillRectangleWithColor(rect, GColor(cBg));
		if (sc.Graphics.SetBackgroundMode != nullptr) sc.Graphics.SetBackgroundMode(TRANSPARENT);
		if (sc.Graphics.SetTextAlign != nullptr) sc.Graphics.SetTextAlign(TA_LEFT | TA_TOP | TA_NOUPDATECP);

		HudPainter P(sc, px, py + 5, panelW, lineH, fontPt, cText);
		char buf[256];

		// 1. BIAS line
		P.Font(fontPt + 4, true);
		const char* biasTxt = H.bias > 0 ? "BIAS: LONG" : (H.bias < 0 ? "BIAS: SHORT" : "BIAS: NEUTRAL");
		const uint32_t biasCol = H.bias > 0 ? cBull : (H.bias < 0 ? cBear : cNeu);
		P.Text(biasTxt, biasCol);
		P.Font(fontPt, true);
		const char* arrow = H.dcsTrend > 5 ? "^" : (H.dcsTrend < -5 ? "v" : "-");
		sprintf_s(buf, sizeof(buf), "DCS %+.0f  %s", H.dcs, arrow);
		P.Text(buf, H.dcs > 0 ? cBull : (H.dcs < 0 ? cBear : cNeu), panelW / 2 + 10);
		P.y += lineH + 6;

		// 2. regime / open / value
		P.Font(fontPt, false);
		const uint32_t regCol = (H.regime == RG_TREND_UP) ? cBull : (H.regime == RG_TREND_DOWN ? cBear : cNeu);
		sprintf_s(buf, sizeof(buf), "%-14s %s", kRegimeNames[Clamp(H.regime, 0, 4)], full ? kOpenTypeNames[Clamp(H.openType, 0, 7)] : "");
		P.Line(buf, regCol);
		if (full)
		{
			const char* vm = H.valueMig > 0.5f ? "Value HIGHER" : (H.valueMig < -0.5f ? "Value LOWER" : "Value OVERLAP");
			sprintf_s(buf, sizeof(buf), "%s   %s", vm, H.vwapText);
			P.Line(buf, H.valueMig > 0.5f ? cBull : (H.valueMig < -0.5f ? cBear : cNeu));
		}

		// 3. MTF strip
		{
			static const char* names[4] = { "1m", "5m", "15m", "60m" };
			int cx = px + 8;
			P.Text("MTF", cText);
			cx += P.TextWidth("MTF ") + 6;
			for (int k = 0; k < 4; ++k)
			{
				const uint32_t c = !H.mtfAvail[k] ? RGB(70, 70, 70) : (H.mtf[k] > 0 ? cBull : (H.mtf[k] < 0 ? cBear : cNeu));
				P.Box(cx, P.y + 2, cx + 34, P.y + lineH - 3, c);
				sc.Graphics.SetTextColor(GColor(RGB(0, 0, 0)));
				sc.Graphics.DrawTextAt(SCString(names[k]), cx + 6, P.y + 1);
				cx += 40;
			}
			P.y += lineH;
		}

		// 4. intermarket
		if (sc.Input[HI_SHOW_INTER].GetYesNo())
		{
			int cx = px + 8;
			P.Text("MKT", cText); cx += P.TextWidth("MKT ") + 6;
			struct Item { const char* name; int v; bool avail; };
			Item items[8]; int cnt = 0;
			items[cnt++] = { "YM", H.ym, H.ymAvail };
			items[cnt++] = { "TICK", H.tick, H.tickAvail };
			for (int k = 0; k < H.megaCount && k < 6; ++k) items[cnt++] = { H.megaNames[k], H.mega[k], true };
			for (int k = 0; k < cnt; ++k)
			{
				const uint32_t c = !items[k].avail ? RGB(70, 70, 70) : (items[k].v > 0 ? cBull : (items[k].v < 0 ? cBear : cNeu));
				P.Dot(cx + 5, P.y + lineH / 2, 4, c);
				sc.Graphics.SetTextColor(GColor(cText));
				sc.Graphics.DrawTextAt(SCString(items[k].name), cx + 12, P.y);
				cx += 12 + P.TextWidth(items[k].name) + 10;
			}
			if (H.smt != 0) { sc.Graphics.SetTextColor(GColor(cWarn)); sc.Graphics.DrawTextAt(SCString(H.smt > 0 ? "SMT+" : "SMT-"), cx + 4, P.y); }
			P.y += lineH;
		}

		// 5. order flow
		if (sc.Input[HI_SHOW_FLOW].GetYesNo())
		{
			const char* cvdT = H.cvdTrend > 0 ? "CVD up" : (H.cvdTrend < 0 ? "CVD down" : "CVD flat");
			if (H.lastEvent[0]) sprintf_s(buf, sizeof(buf), "%s (z%+.1f)  %s @ %s", cvdT, H.cvdZ, H.lastEvent, sc.FormatGraphValue(H.lastEventPrice, sc.BaseGraphValueFormat).GetChars());
			else sprintf_s(buf, sizeof(buf), "%s (z%+.1f)", cvdT, H.cvdZ);
			P.Line(buf, H.cvdTrend > 0 ? cBull : (H.cvdTrend < 0 ? cBear : cNeu));
		}

		// 6. nearest levels
		if (sc.Input[HI_SHOW_LEVELS].GetYesNo())
		{
			const float ts = S.tickSize > 0 ? S.tickSize : 0.25f;
			if (H.resPrice > 0 && H.supPrice > 0)
				sprintf_s(buf, sizeof(buf), "R %s +%d t (%.1f A) | S %s -%d t (%.1f A)",
					sc.FormatGraphValue(H.resPrice, sc.BaseGraphValueFormat).GetChars(), static_cast<int>((H.resPrice - H.close) / ts + 0.5f), H.atr > 0 ? (H.resPrice - H.close) / H.atr : 0,
					sc.FormatGraphValue(H.supPrice, sc.BaseGraphValueFormat).GetChars(), static_cast<int>((H.close - H.supPrice) / ts + 0.5f), H.atr > 0 ? (H.close - H.supPrice) / H.atr : 0);
			else sprintf_s(buf, sizeof(buf), "Levels: waiting for session data");
			P.Line(buf, cLevel);
		}

		// 7. state line
		P.Font(fontPt, true);
		P.Line(H.stateLine[0] ? H.stateLine : "Warming up...", cVwap);
		P.Font(fontPt, false);

		// 8. stats
		if (full && sc.Input[HI_SHOW_STATS].GetYesNo())
		{
			if (H.statsAvail && H.curStats.count > 0)
			{
				const SetupStats& st = H.curStats;
				const double pf = st.sumLossR < 0 ? st.sumWinR / -st.sumLossR : (st.sumWinR > 0 ? 99.0 : 0.0);
				sprintf_s(buf, sizeof(buf), "%s: n=%d win %.0f%% avgR %+.2f PF %.2f%s", kSetupNames[Clamp(H.curSetup, 0, SETUP_COUNT - 1)], st.count,
					100.0 * st.wins / Max(1, st.wins + st.losses), st.sumR / st.count, pf, st.count < S.params.val.minSample ? "  (small n)" : "");
				P.Line(buf, st.count < S.params.val.minSample ? cWarn : cText);
				sprintf_s(buf, sizeof(buf), "T2 %.0f%%  MFE %.2fR  MAE %.2fR  %d signals total", 100.0 * st.t2 / st.count, st.sumMfe / st.count, st.sumMae / st.count, H.signalsTotal);
				P.Line(buf, cText);
			}
			else { P.Line("Stats: no resolved signals yet", cNeu); P.Line("", cNeu); }
		}

		// 9. warnings
		if (sc.Input[HI_SHOW_WARN].GetYesNo() && S.warn.text[0])
		{
			const char* p = S.warn.text;
			while (*p)
			{
				const char* e = strchr(p, '\n'); if (!e) e = p + strlen(p);
				std::string ln(p, e);
				P.Line(ln.c_str(), cWarn);
				p = *e ? e + 1 : e;
			}
		}
	}
} // namespace nqe

using namespace nqe;

// ==== 15 Study functions ======================================================

// Helper: colour input declaration
#define NQE_COLOR_INPUT(IDX, NAME, R, G, B) { sc.Input[IDX].Name = NAME; sc.Input[IDX].SetColor(RGB(R, G, B)); }
#define NQE_YESNO_INPUT(IDX, NAME, V)       { sc.Input[IDX].Name = NAME; sc.Input[IDX].SetYesNo(V); }
#define NQE_INT_INPUT(IDX, NAME, V, LO, HI) { sc.Input[IDX].Name = NAME; sc.Input[IDX].SetInt(V); sc.Input[IDX].SetIntLimits(LO, HI); }
#define NQE_FLT_INPUT(IDX, NAME, V, LO, HI) { sc.Input[IDX].Name = NAME; sc.Input[IDX].SetFloat(static_cast<float>(V)); sc.Input[IDX].SetFloatLimits(static_cast<float>(LO), static_cast<float>(HI)); }

// --- 1. Auction / Structure ---------------------------------------------------
enum AuctionInput
{
	AI_RTH_START = 0, AI_RTH_END, AI_ATR_LEN, AI_IB_MIN, AI_OPEN_MIN, AI_VA_PCT, AI_PROF_TICKS, AI_NAKED_N, AI_TPO_MIN,
	AI_SWING_N, AI_SWING_ATR, AI_EQ_TOL, AI_BOS_DECAY, AI_IBEXT_A, AI_IBEXT_B, AI_IBEXT_C,
	AI_D_DEVVA, AI_D_PDVA, AI_D_NAKED, AI_D_SINGLE, AI_D_ON, AI_D_IB, AI_D_PDHL, AI_D_SWING, AI_D_BOS, AI_D_LIQ, AI_MAX_ZONES,
	AI_C_POC, AI_C_VA, AI_C_PD, AI_C_NAKED, AI_C_SINGLE, AI_C_ON, AI_C_IB, AI_C_IBEXT, AI_C_SWH, AI_C_SWL, AI_C_BOS, AI_C_CHOCH, AI_C_LIQ, AI_COUNT
};
enum AuctionSubgraph
{
	AS_POC = 0, AS_VAH, AS_VAL, AS_PDPOC, AS_PDVAH, AS_PDVAL, AS_ONH, AS_ONL, AS_IBH, AS_IBL, AS_IBEXT_AU, AS_IBEXT_AD, AS_IBEXT_BU, AS_IBEXT_BD,
	AS_IBEXT_CU, AS_IBEXT_CD, AS_PDH, AS_PDL, AS_SWH, AS_SWL, AS_BOSU, AS_BOSD, AS_CHU, AS_CHD, AS_F_STRUCT, AS_F_BOS, AS_F_VAPOS, AS_F_POCPOS,
	AS_F_IBPOS, AS_F_VALMIG, AS_F_OPEN, AS_COUNT
};

SCSFExport scsf_NQEdge_Auction(SCStudyInterfaceRef sc)
{
	if (sc.SetDefaults)
	{
		sc.GraphName = "NQ Edge: Auction/Structure Engine";
		sc.StudyDescription = "Developing/prior-day volume profile, naked POCs, single prints, ON range, IB, swings, BOS/CHoCH, liquidity pools. Owns the session and ATR settings for the whole suite.";
		sc.GraphRegion = 0; sc.AutoLoop = 0; sc.CalculationPrecedence = STD_PREC_LEVEL; sc.ValueFormat = VALUEFORMAT_INHERITED;
		sc.MaintainVolumeAtPriceData = 1; sc.ScaleRangeType = SCALE_SAMEASREGION; sc.DrawZeros = 0;

		const char* names[AS_COUNT] = { "Dev POC", "Dev VAH", "Dev VAL", "PD POC", "PD VAH", "PD VAL", "ON High", "ON Low", "IB High", "IB Low",
			"IB Ext A Up", "IB Ext A Dn", "IB Ext B Up", "IB Ext B Dn", "IB Ext C Up", "IB Ext C Dn", "PDH", "PDL", "Swing High", "Swing Low",
			"BOS Up", "BOS Down", "CHoCH Up", "CHoCH Down", "f.structTrend", "f.bos", "f.vaPos", "f.pocPos", "f.ibPos", "f.valueMig", "f.openType" };
		for (int k = 0; k < AS_COUNT; ++k)
		{
			sc.Subgraph[k].Name = names[k]; sc.Subgraph[k].DrawZeros = 0; sc.Subgraph[k].LineWidth = 1;
			sc.Subgraph[k].DrawStyle = (k >= AS_F_STRUCT) ? DRAWSTYLE_IGNORE : DRAWSTYLE_DASH;
		}
		sc.Subgraph[AS_POC].DrawStyle = DRAWSTYLE_LINE; sc.Subgraph[AS_POC].LineWidth = 2;
		sc.Subgraph[AS_SWH].DrawStyle = DRAWSTYLE_TRIANGLE_DOWN; sc.Subgraph[AS_SWL].DrawStyle = DRAWSTYLE_TRIANGLE_UP;
		sc.Subgraph[AS_BOSU].DrawStyle = DRAWSTYLE_ARROW_UP; sc.Subgraph[AS_BOSD].DrawStyle = DRAWSTYLE_ARROW_DOWN;
		sc.Subgraph[AS_CHU].DrawStyle = DRAWSTYLE_DIAMOND; sc.Subgraph[AS_CHD].DrawStyle = DRAWSTYLE_DIAMOND;
		sc.Subgraph[AS_SWH].LineWidth = 3; sc.Subgraph[AS_SWL].LineWidth = 3; sc.Subgraph[AS_BOSU].LineWidth = 3; sc.Subgraph[AS_BOSD].LineWidth = 3; sc.Subgraph[AS_CHU].LineWidth = 3; sc.Subgraph[AS_CHD].LineWidth = 3;

		sc.Input[AI_RTH_START].Name = "Session: RTH Start Time"; sc.Input[AI_RTH_START].SetTime(HMS_TIME(9, 30, 0));
		sc.Input[AI_RTH_END].Name = "Session: RTH End Time"; sc.Input[AI_RTH_END].SetTime(HMS_TIME(16, 0, 0));
		NQE_INT_INPUT(AI_ATR_LEN, "Session: ATR Length (suite-wide)", 14, 2, 500);
		NQE_INT_INPUT(AI_IB_MIN, "Profile: Initial Balance Minutes", 60, 5, 240);
		NQE_INT_INPUT(AI_OPEN_MIN, "Profile: Open Type Evaluation Minutes", 30, 5, 120);
		NQE_FLT_INPUT(AI_VA_PCT, "Profile: Value Area Percent", 70.0, 50.0, 95.0);
		NQE_INT_INPUT(AI_PROF_TICKS, "Profile: Ticks Per Level", 1, 1, 20);
		NQE_INT_INPUT(AI_NAKED_N, "Profile: Naked POCs To Track", 10, 0, 50);
		NQE_INT_INPUT(AI_TPO_MIN, "Profile: TPO Period Minutes (single prints)", 30, 5, 120);
		NQE_INT_INPUT(AI_SWING_N, "Structure: Swing Strength Bars", 5, 2, 50);
		NQE_FLT_INPUT(AI_SWING_ATR, "Structure: Swing Min Distance (ATR)", 0.5, 0.0, 10.0);
		NQE_INT_INPUT(AI_EQ_TOL, "Structure: Equal High/Low Tolerance (ticks)", 2, 0, 50);
		NQE_INT_INPUT(AI_BOS_DECAY, "Structure: BOS/CHoCH Decay Bars", 10, 1, 200);
		NQE_FLT_INPUT(AI_IBEXT_A, "IB Extension Multiple A", 0.5, 0.0, 5.0);
		NQE_FLT_INPUT(AI_IBEXT_B, "IB Extension Multiple B", 1.0, 0.0, 5.0);
		NQE_FLT_INPUT(AI_IBEXT_C, "IB Extension Multiple C", 2.0, 0.0, 5.0);
		NQE_YESNO_INPUT(AI_D_DEVVA, "Draw: Developing POC/VAH/VAL", 1);
		NQE_YESNO_INPUT(AI_D_PDVA, "Draw: Prior Day POC/VAH/VAL", 1);
		NQE_YESNO_INPUT(AI_D_NAKED, "Draw: Naked POCs", 1);
		NQE_YESNO_INPUT(AI_D_SINGLE, "Draw: Single Prints", 1);
		NQE_YESNO_INPUT(AI_D_ON, "Draw: Overnight High/Low", 1);
		NQE_YESNO_INPUT(AI_D_IB, "Draw: IB and Extensions", 1);
		NQE_YESNO_INPUT(AI_D_PDHL, "Draw: Prior Day High/Low", 1);
		NQE_YESNO_INPUT(AI_D_SWING, "Draw: Swing Points", 1);
		NQE_YESNO_INPUT(AI_D_BOS, "Draw: BOS/CHoCH Markers", 1);
		NQE_YESNO_INPUT(AI_D_LIQ, "Draw: Liquidity Pools", 1);
		NQE_INT_INPUT(AI_MAX_ZONES, "Draw: Max Zone Drawings", 40, 5, 200);
		NQE_COLOR_INPUT(AI_C_POC, "Color: Developing POC", 0, 200, 255);
		NQE_COLOR_INPUT(AI_C_VA, "Color: Developing VAH/VAL", 0, 150, 190);
		NQE_COLOR_INPUT(AI_C_PD, "Color: Prior Day POC/VA", 120, 120, 160);
		NQE_COLOR_INPUT(AI_C_NAKED, "Color: Naked POC", 255, 120, 255);
		NQE_COLOR_INPUT(AI_C_SINGLE, "Color: Single Prints", 160, 120, 60);
		NQE_COLOR_INPUT(AI_C_ON, "Color: Overnight High/Low", 90, 140, 200);
		NQE_COLOR_INPUT(AI_C_IB, "Color: Initial Balance", 230, 200, 60);
		NQE_COLOR_INPUT(AI_C_IBEXT, "Color: IB Extensions", 140, 120, 40);
		NQE_COLOR_INPUT(AI_C_SWH, "Color: Swing High", 220, 80, 80);
		NQE_COLOR_INPUT(AI_C_SWL, "Color: Swing Low", 80, 220, 120);
		NQE_COLOR_INPUT(AI_C_BOS, "Color: BOS", 255, 255, 255);
		NQE_COLOR_INPUT(AI_C_CHOCH, "Color: CHoCH", 255, 160, 0);
		NQE_COLOR_INPUT(AI_C_LIQ, "Color: Liquidity Pool", 0, 220, 220);
		return;
	}
	if (sc.LastCallToFunction) { Release(sc, E_AUCTION); return; }
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	ChartState& S = Acquire(sc);
	S.studyPresent[E_AUCTION] = true;
	if (sc.MaintainVolumeAtPriceData == 0) { sc.MaintainVolumeAtPriceData = 1; sc.FlagToReloadChartData = 1; }

	BaseParams bp{}; bp.rthStartSec = sc.Input[AI_RTH_START].GetTime(); bp.rthEndSec = sc.Input[AI_RTH_END].GetTime(); bp.atrLength = sc.Input[AI_ATR_LEN].GetInt();
	SetParams(S, E_BASE, S.params.base, bp);
	AuctionParams ap{};
	ap.ibMinutes = sc.Input[AI_IB_MIN].GetInt(); ap.openTypeMinutes = sc.Input[AI_OPEN_MIN].GetInt(); ap.valueAreaPct = sc.Input[AI_VA_PCT].GetFloat();
	ap.profileTicksPerLevel = sc.Input[AI_PROF_TICKS].GetInt(); ap.nakedPocsTracked = sc.Input[AI_NAKED_N].GetInt(); ap.tpoMinutes = sc.Input[AI_TPO_MIN].GetInt();
	ap.swingStrength = sc.Input[AI_SWING_N].GetInt(); ap.swingMinAtr = sc.Input[AI_SWING_ATR].GetFloat(); ap.equalTolTicks = sc.Input[AI_EQ_TOL].GetInt();
	ap.bosDecayBars = sc.Input[AI_BOS_DECAY].GetInt(); ap.ibExtA = sc.Input[AI_IBEXT_A].GetFloat(); ap.ibExtB = sc.Input[AI_IBEXT_B].GetFloat(); ap.ibExtC = sc.Input[AI_IBEXT_C].GetFloat();
	SetParams(S, E_AUCTION, S.params.auction, ap);

	CheckDataStamp(sc, S);
	CheckWarnings(sc, S);
	EnsureAuction(sc, S);

	// Subgraph mirror
	int& gen = sc.GetPersistentInt(1);
	int start = Min(sc.UpdateStartIndex, S.auction.dirtyFrom); S.auction.dirtyFrom = INT_MAX;
	if (gen != S.auction.generation) { start = 0; gen = S.auction.generation; }
	if (start < 0) start = 0;
	const AuctionState& A = S.auction; const AuctionParams& P = S.params.auction;
	const bool dDev = sc.Input[AI_D_DEVVA].GetYesNo() != 0, dPd = sc.Input[AI_D_PDVA].GetYesNo() != 0, dOn = sc.Input[AI_D_ON].GetYesNo() != 0, dIb = sc.Input[AI_D_IB].GetYesNo() != 0;
	const bool dPdhl = sc.Input[AI_D_PDHL].GetYesNo() != 0, dSw = sc.Input[AI_D_SWING].GetYesNo() != 0, dBos = sc.Input[AI_D_BOS].GetYesNo() != 0;
	sc.Subgraph[AS_POC].PrimaryColor = sc.Input[AI_C_POC].GetColor(); sc.Subgraph[AS_VAH].PrimaryColor = sc.Subgraph[AS_VAL].PrimaryColor = sc.Input[AI_C_VA].GetColor();
	sc.Subgraph[AS_PDPOC].PrimaryColor = sc.Subgraph[AS_PDVAH].PrimaryColor = sc.Subgraph[AS_PDVAL].PrimaryColor = sc.Input[AI_C_PD].GetColor();
	sc.Subgraph[AS_ONH].PrimaryColor = sc.Subgraph[AS_ONL].PrimaryColor = sc.Input[AI_C_ON].GetColor();
	sc.Subgraph[AS_IBH].PrimaryColor = sc.Subgraph[AS_IBL].PrimaryColor = sc.Input[AI_C_IB].GetColor();
	for (int k = AS_IBEXT_AU; k <= AS_IBEXT_CD; ++k) sc.Subgraph[k].PrimaryColor = sc.Input[AI_C_IBEXT].GetColor();
	sc.Subgraph[AS_PDH].PrimaryColor = sc.Subgraph[AS_PDL].PrimaryColor = sc.Input[AI_C_PD].GetColor();
	sc.Subgraph[AS_SWH].PrimaryColor = sc.Input[AI_C_SWH].GetColor(); sc.Subgraph[AS_SWL].PrimaryColor = sc.Input[AI_C_SWL].GetColor();
	sc.Subgraph[AS_BOSU].PrimaryColor = sc.Subgraph[AS_BOSD].PrimaryColor = sc.Input[AI_C_BOS].GetColor();
	sc.Subgraph[AS_CHU].PrimaryColor = sc.Subgraph[AS_CHD].PrimaryColor = sc.Input[AI_C_CHOCH].GetColor();
	sc.Subgraph[AS_POC].DrawStyle = dDev ? DRAWSTYLE_LINE : DRAWSTYLE_IGNORE; sc.Subgraph[AS_VAH].DrawStyle = sc.Subgraph[AS_VAL].DrawStyle = dDev ? DRAWSTYLE_DASH : DRAWSTYLE_IGNORE;
	sc.Subgraph[AS_PDPOC].DrawStyle = sc.Subgraph[AS_PDVAH].DrawStyle = sc.Subgraph[AS_PDVAL].DrawStyle = dPd ? DRAWSTYLE_DASH : DRAWSTYLE_IGNORE;
	sc.Subgraph[AS_ONH].DrawStyle = sc.Subgraph[AS_ONL].DrawStyle = dOn ? DRAWSTYLE_DASH : DRAWSTYLE_IGNORE;
	sc.Subgraph[AS_IBH].DrawStyle = sc.Subgraph[AS_IBL].DrawStyle = dIb ? DRAWSTYLE_LINE : DRAWSTYLE_IGNORE;
	for (int k = AS_IBEXT_AU; k <= AS_IBEXT_CD; ++k) sc.Subgraph[k].DrawStyle = dIb ? DRAWSTYLE_DASH : DRAWSTYLE_IGNORE;
	sc.Subgraph[AS_PDH].DrawStyle = sc.Subgraph[AS_PDL].DrawStyle = dPdhl ? DRAWSTYLE_LINE : DRAWSTYLE_IGNORE;
	sc.Subgraph[AS_SWH].DrawStyle = dSw ? DRAWSTYLE_TRIANGLE_DOWN : DRAWSTYLE_IGNORE; sc.Subgraph[AS_SWL].DrawStyle = dSw ? DRAWSTYLE_TRIANGLE_UP : DRAWSTYLE_IGNORE;
	sc.Subgraph[AS_BOSU].DrawStyle = dBos ? DRAWSTYLE_ARROW_UP : DRAWSTYLE_IGNORE; sc.Subgraph[AS_BOSD].DrawStyle = dBos ? DRAWSTYLE_ARROW_DOWN : DRAWSTYLE_IGNORE;
	sc.Subgraph[AS_CHU].DrawStyle = dBos ? DRAWSTYLE_DIAMOND : DRAWSTYLE_IGNORE; sc.Subgraph[AS_CHD].DrawStyle = dBos ? DRAWSTYLE_DIAMOND : DRAWSTYLE_IGNORE;

	for (int i = start; i < sc.ArraySize; ++i)
	{
		sc.Subgraph[AS_POC][i] = A.poc[i]; sc.Subgraph[AS_VAH][i] = A.vah[i]; sc.Subgraph[AS_VAL][i] = A.val[i];
		sc.Subgraph[AS_PDPOC][i] = A.pdPoc[i]; sc.Subgraph[AS_PDVAH][i] = A.pdVah[i]; sc.Subgraph[AS_PDVAL][i] = A.pdVal[i];
		sc.Subgraph[AS_ONH][i] = A.onHigh[i]; sc.Subgraph[AS_ONL][i] = A.onLow[i];
		const float ibh = A.ibHigh[i], ibl = A.ibLow[i]; const float r = ibh - ibl;
		sc.Subgraph[AS_IBH][i] = ibh; sc.Subgraph[AS_IBL][i] = ibl;
		const bool ext = A.ibDone[i] != 0 && ibh > 0;
		sc.Subgraph[AS_IBEXT_AU][i] = ext && P.ibExtA > 0 ? ibh + r * P.ibExtA : 0; sc.Subgraph[AS_IBEXT_AD][i] = ext && P.ibExtA > 0 ? ibl - r * P.ibExtA : 0;
		sc.Subgraph[AS_IBEXT_BU][i] = ext && P.ibExtB > 0 ? ibh + r * P.ibExtB : 0; sc.Subgraph[AS_IBEXT_BD][i] = ext && P.ibExtB > 0 ? ibl - r * P.ibExtB : 0;
		sc.Subgraph[AS_IBEXT_CU][i] = ext && P.ibExtC > 0 ? ibh + r * P.ibExtC : 0; sc.Subgraph[AS_IBEXT_CD][i] = ext && P.ibExtC > 0 ? ibl - r * P.ibExtC : 0;
		sc.Subgraph[AS_PDH][i] = A.pdh[i]; sc.Subgraph[AS_PDL][i] = A.pdl[i];
		sc.Subgraph[AS_SWH][i] = A.swingHighMark[i] ? sc.High[i] : 0.0f; sc.Subgraph[AS_SWL][i] = A.swingLowMark[i] ? sc.Low[i] : 0.0f;
		sc.Subgraph[AS_BOSU][i] = A.bosMark[i] > 0 ? sc.Low[i] : 0.0f; sc.Subgraph[AS_BOSD][i] = A.bosMark[i] < 0 ? sc.High[i] : 0.0f;
		sc.Subgraph[AS_CHU][i] = A.chochMark[i] > 0 ? sc.Low[i] : 0.0f; sc.Subgraph[AS_CHD][i] = A.chochMark[i] < 0 ? sc.High[i] : 0.0f;
		sc.Subgraph[AS_F_STRUCT][i] = A.structTrend[i]; sc.Subgraph[AS_F_BOS][i] = A.bos[i]; sc.Subgraph[AS_F_VAPOS][i] = A.vaPos[i];
		sc.Subgraph[AS_F_POCPOS][i] = A.pocPos[i]; sc.Subgraph[AS_F_IBPOS][i] = A.ibPos[i]; sc.Subgraph[AS_F_VALMIG][i] = A.valueMig[i]; sc.Subgraph[AS_F_OPEN][i] = A.openTypeDir[i];
	}

	// Drawings owned by this study: naked POC rays, single-print zones, liquidity pools
	AuctionState& AW = S.auction;
	for (size_t k = 0; k < AW.deadLines.size(); ++k) { int ln = AW.deadLines[k]; DeleteDrawing(sc, ln); }
	AW.deadLines.clear();
	const int last = sc.ArraySize - 1;
	const int maxZones = Max(5, sc.Input[AI_MAX_ZONES].GetInt());
	if (sc.Input[AI_D_NAKED].GetYesNo())
	{
		for (size_t k = 0; k < AW.nakedPocs.size(); ++k)
		{
			char txt[32]; sprintf_s(txt, sizeof(txt), "nPOC");
			DrawRay(sc, AW.nakedPocLine[k], AW.nakedPocBorn[k], AW.nakedPocs[k], sc.Input[AI_C_NAKED].GetColor(), 1, LINESTYLE_DOT, txt);
		}
	}
	else for (size_t k = 0; k < AW.nakedPocLine.size(); ++k) DeleteDrawing(sc, AW.nakedPocLine[k]);
	if (sc.Input[AI_D_SINGLE].GetYesNo())
	{
		int drawn = 0;
		for (size_t z = 0; z < AW.singlePrints.size() && drawn < maxZones; ++z, ++drawn)
			DrawRect(sc, AW.singlePrints[z].lineNumber, AW.singlePrints[z].bornIdx, last, AW.singlePrints[z].top, AW.singlePrints[z].bottom, sc.Input[AI_C_SINGLE].GetColor(), 75, "single prints");
	}
	else for (size_t z = 0; z < AW.singlePrints.size(); ++z) DeleteDrawing(sc, AW.singlePrints[z].lineNumber);
	if (sc.Input[AI_D_LIQ].GetYesNo())
	{
		int drawn = 0;
		for (int z = static_cast<int>(AW.liquidity.size()) - 1; z >= 0; --z)
		{
			Zone& Z = AW.liquidity[z];
			if (!Z.active) { if (Z.deadIdx >= 0 && last - Z.deadIdx > 5) DeleteDrawing(sc, Z.lineNumber); else if (Z.lineNumber) DrawRect(sc, Z.lineNumber, Z.bornIdx, Z.deadIdx, Z.top, Z.bottom, sc.Input[AI_C_LIQ].GetColor(), 85); continue; }
			if (drawn++ >= maxZones) { DeleteDrawing(sc, Z.lineNumber); continue; }
			DrawRect(sc, Z.lineNumber, Z.bornIdx, last, Z.top, Z.bottom, sc.Input[AI_C_LIQ].GetColor(), 70, Z.kind == LVL_LIQ_EQH ? "EQH liquidity" : "EQL liquidity");
		}
	}
	else for (size_t z = 0; z < AW.liquidity.size(); ++z) DeleteDrawing(sc, AW.liquidity[z].lineNumber);
}

// --- 2. VWAP ------------------------------------------------------------------
enum VwapInput { VI_ANCHOR = 0, VI_B1, VI_B2, VI_B3, VI_SLOPE, VI_ACCEPT, VI_D_SESSION, VI_D_ON, VI_D_RTH, VI_D_SWING, VI_C_VWAP, VI_C_B1, VI_C_B2, VI_C_B3, VI_C_ON, VI_C_RTH, VI_C_SWH, VI_C_SWL, VI_COUNT };
enum VwapSubgraph { VS_VWAP = 0, VS_B1U, VS_B1D, VS_B2U, VS_B2D, VS_B3U, VS_B3D, VS_ON, VS_RTH, VS_SWH, VS_SWL, VS_F_SLOPE, VS_F_POS, VS_F_ACCEPT, VS_COUNT };

SCSFExport scsf_NQEdge_VWAP(SCStudyInterfaceRef sc)
{
	if (sc.SetDefaults)
	{
		sc.GraphName = "NQ Edge: VWAP Engine";
		sc.StudyDescription = "Session VWAP with variance bands, anchored VWAPs (overnight open, RTH open, last swing high/low), ATR-normalized slope and acceptance state.";
		sc.GraphRegion = 0; sc.AutoLoop = 0; sc.CalculationPrecedence = STD_PREC_LEVEL; sc.ValueFormat = VALUEFORMAT_INHERITED; sc.ScaleRangeType = SCALE_SAMEASREGION; sc.DrawZeros = 0;
		const char* names[VS_COUNT] = { "VWAP", "+1 SD", "-1 SD", "+2 SD", "-2 SD", "+3 SD", "-3 SD", "AVWAP ON", "AVWAP RTH", "AVWAP Swing High", "AVWAP Swing Low", "f.vwapSlope", "f.vwapPos", "f.vwapAccept" };
		for (int k = 0; k < VS_COUNT; ++k) { sc.Subgraph[k].Name = names[k]; sc.Subgraph[k].DrawZeros = 0; sc.Subgraph[k].DrawStyle = k >= VS_F_SLOPE ? DRAWSTYLE_IGNORE : DRAWSTYLE_LINE; sc.Subgraph[k].LineWidth = 1; }
		sc.Subgraph[VS_VWAP].LineWidth = 2;
		sc.Input[VI_ANCHOR].Name = "Session VWAP Anchor"; sc.Input[VI_ANCHOR].SetCustomInputStrings("RTH Open;Trading Day Start"); sc.Input[VI_ANCHOR].SetCustomInputIndex(0);
		NQE_FLT_INPUT(VI_B1, "Band 1 Std Dev Multiplier", 1.0, 0.1, 10.0);
		NQE_FLT_INPUT(VI_B2, "Band 2 Std Dev Multiplier", 2.0, 0.1, 10.0);
		NQE_FLT_INPUT(VI_B3, "Band 3 Std Dev Multiplier", 3.0, 0.1, 10.0);
		NQE_INT_INPUT(VI_SLOPE, "Slope Lookback Bars", 10, 1, 200);
		NQE_INT_INPUT(VI_ACCEPT, "Acceptance Closes", 3, 1, 20);
		NQE_YESNO_INPUT(VI_D_SESSION, "Draw: Session VWAP + Bands", 1);
		NQE_YESNO_INPUT(VI_D_ON, "Draw: Overnight-anchored VWAP", 1);
		NQE_YESNO_INPUT(VI_D_RTH, "Draw: RTH-anchored VWAP", 0);
		NQE_YESNO_INPUT(VI_D_SWING, "Draw: Swing-anchored VWAPs", 1);
		NQE_COLOR_INPUT(VI_C_VWAP, "Color: VWAP", 255, 215, 0);
		NQE_COLOR_INPUT(VI_C_B1, "Color: Band 1", 190, 170, 60);
		NQE_COLOR_INPUT(VI_C_B2, "Color: Band 2", 150, 130, 50);
		NQE_COLOR_INPUT(VI_C_B3, "Color: Band 3", 110, 95, 40);
		NQE_COLOR_INPUT(VI_C_ON, "Color: AVWAP Overnight", 120, 170, 255);
		NQE_COLOR_INPUT(VI_C_RTH, "Color: AVWAP RTH", 255, 170, 90);
		NQE_COLOR_INPUT(VI_C_SWH, "Color: AVWAP Swing High", 230, 100, 100);
		NQE_COLOR_INPUT(VI_C_SWL, "Color: AVWAP Swing Low", 100, 230, 140);
		return;
	}
	if (sc.LastCallToFunction) { Release(sc, E_VWAP); return; }
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	ChartState& S = Acquire(sc);
	S.studyPresent[E_VWAP] = true;
	VwapParams vp{}; vp.anchor = sc.Input[VI_ANCHOR].GetIndex(); vp.band1 = sc.Input[VI_B1].GetFloat(); vp.band2 = sc.Input[VI_B2].GetFloat(); vp.band3 = sc.Input[VI_B3].GetFloat();
	vp.slopeBars = sc.Input[VI_SLOPE].GetInt(); vp.acceptCloses = sc.Input[VI_ACCEPT].GetInt();
	SetParams(S, E_VWAP, S.params.vwap, vp);
	CheckDataStamp(sc, S);
	EnsureVwap(sc, S);

	// colours + visibility follow inputs
	sc.Subgraph[VS_VWAP].PrimaryColor = sc.Input[VI_C_VWAP].GetColor();
	sc.Subgraph[VS_B1U].PrimaryColor = sc.Subgraph[VS_B1D].PrimaryColor = sc.Input[VI_C_B1].GetColor();
	sc.Subgraph[VS_B2U].PrimaryColor = sc.Subgraph[VS_B2D].PrimaryColor = sc.Input[VI_C_B2].GetColor();
	sc.Subgraph[VS_B3U].PrimaryColor = sc.Subgraph[VS_B3D].PrimaryColor = sc.Input[VI_C_B3].GetColor();
	sc.Subgraph[VS_ON].PrimaryColor = sc.Input[VI_C_ON].GetColor(); sc.Subgraph[VS_RTH].PrimaryColor = sc.Input[VI_C_RTH].GetColor();
	sc.Subgraph[VS_SWH].PrimaryColor = sc.Input[VI_C_SWH].GetColor(); sc.Subgraph[VS_SWL].PrimaryColor = sc.Input[VI_C_SWL].GetColor();
	const bool dS = sc.Input[VI_D_SESSION].GetYesNo() != 0, dO = sc.Input[VI_D_ON].GetYesNo() != 0, dR = sc.Input[VI_D_RTH].GetYesNo() != 0, dW = sc.Input[VI_D_SWING].GetYesNo() != 0;
	for (int k = VS_VWAP; k <= VS_B3D; ++k) sc.Subgraph[k].DrawStyle = dS ? DRAWSTYLE_LINE : DRAWSTYLE_IGNORE;
	sc.Subgraph[VS_ON].DrawStyle = dO ? DRAWSTYLE_LINE : DRAWSTYLE_IGNORE; sc.Subgraph[VS_RTH].DrawStyle = dR ? DRAWSTYLE_LINE : DRAWSTYLE_IGNORE;
	sc.Subgraph[VS_SWH].DrawStyle = dW ? DRAWSTYLE_DASH : DRAWSTYLE_IGNORE; sc.Subgraph[VS_SWL].DrawStyle = dW ? DRAWSTYLE_DASH : DRAWSTYLE_IGNORE;

	int& gen = sc.GetPersistentInt(1);
	int start = Min(sc.UpdateStartIndex, S.vwap.dirtyFrom); S.vwap.dirtyFrom = INT_MAX;
	if (gen != S.vwap.generation) { start = 0; gen = S.vwap.generation; }
	if (start < 0) start = 0;
	const VwapState& V = S.vwap;
	for (int i = start; i < sc.ArraySize; ++i)
	{
		sc.Subgraph[VS_VWAP][i] = V.vwap[i]; sc.Subgraph[VS_B1U][i] = V.b1u[i]; sc.Subgraph[VS_B1D][i] = V.b1d[i]; sc.Subgraph[VS_B2U][i] = V.b2u[i]; sc.Subgraph[VS_B2D][i] = V.b2d[i];
		sc.Subgraph[VS_B3U][i] = V.b3u[i]; sc.Subgraph[VS_B3D][i] = V.b3d[i]; sc.Subgraph[VS_ON][i] = V.avOn[i]; sc.Subgraph[VS_RTH][i] = V.avRth[i];
		sc.Subgraph[VS_SWH][i] = V.avSwHi[i]; sc.Subgraph[VS_SWL][i] = V.avSwLo[i]; sc.Subgraph[VS_F_SLOPE][i] = V.slope[i]; sc.Subgraph[VS_F_POS][i] = V.pos[i]; sc.Subgraph[VS_F_ACCEPT][i] = V.accept[i];
	}
}

// --- 3. Order Flow ------------------------------------------------------------
enum FlowInput
{
	FI_CVD_RESET = 0, FI_CVD_SLOPE, FI_VOLZ_LEN, FI_ABS_Z, FI_ABS_RANGE, FI_ABS_FRAC, FI_EXH_RUN, FI_EXH_PCT, FI_IMB_RATIO, FI_IMB_MINVOL, FI_IMB_STACK,
	FI_LT_PCT, FI_LT_MIN, FI_LT_LOOK, FI_LT_MAX, FI_TRAP_LOOK, FI_TRAP_K, FI_TRAP_DELTA, FI_DIV_ATR, FI_DECAY, FI_MAX_ZONES,
	FI_D_CVD, FI_D_ABS, FI_D_IMB, FI_D_BUBBLE, FI_D_DIV, FI_D_TRAP, FI_D_EXH,
	FI_C_CVD_UP, FI_C_CVD_DN, FI_C_ABS_BULL, FI_C_ABS_BEAR, FI_C_IMB_BUY, FI_C_IMB_SELL, FI_C_BUB_BUY, FI_C_BUB_SELL, FI_C_DIV, FI_C_TRAP, FI_C_EXH, FI_COUNT
};
enum FlowSubgraph { FS_CVD = 0, FS_DELTA, FS_DELTA_PCT, FS_VOLZ, FS_CVDZ, FS_F_ABS, FS_F_EXH, FS_F_IMB, FS_F_TRAP, FS_F_DIV, FS_F_LARGE, FS_COUNT };

SCSFExport scsf_NQEdge_OrderFlow(SCStudyInterfaceRef sc)
{
	if (sc.SetDefaults)
	{
		sc.GraphName = "NQ Edge: Order Flow Engine";
		sc.StudyDescription = "Tick-level delta/CVD, CVD divergence, absorption, exhaustion, stacked imbalances, large trades (T&S + VAP), trapped traders. Zones drawn on the price chart.";
		sc.GraphRegion = 1; sc.AutoLoop = 0; sc.CalculationPrecedence = STD_PREC_LEVEL; sc.ValueFormat = 0; sc.MaintainVolumeAtPriceData = 1; sc.DrawZeros = 0;
		const char* names[FS_COUNT] = { "CVD", "Delta", "Delta %", "Volume Z", "CVD Z", "f.absorb", "f.exhaust", "f.imbalance", "f.trapped", "f.cvdDiv", "f.largeTrade" };
		for (int k = 0; k < FS_COUNT; ++k) { sc.Subgraph[k].Name = names[k]; sc.Subgraph[k].DrawStyle = DRAWSTYLE_IGNORE; sc.Subgraph[k].DrawZeros = 1; }
		sc.Subgraph[FS_CVD].DrawStyle = DRAWSTYLE_LINE; sc.Subgraph[FS_CVD].LineWidth = 2; sc.Subgraph[FS_CVD].PrimaryColor = RGB(0, 200, 120); sc.Subgraph[FS_CVD].SecondaryColor = RGB(220, 70, 70); sc.Subgraph[FS_CVD].SecondaryColorUsed = 1;
		sc.Subgraph[FS_DELTA].DrawStyle = DRAWSTYLE_BAR; sc.Subgraph[FS_DELTA].PrimaryColor = RGB(90, 90, 90);

		sc.Input[FI_CVD_RESET].Name = "CVD Reset"; sc.Input[FI_CVD_RESET].SetCustomInputStrings("RTH Open;Trading Day Start;Never"); sc.Input[FI_CVD_RESET].SetCustomInputIndex(0);
		NQE_INT_INPUT(FI_CVD_SLOPE, "CVD Slope Bars", 10, 2, 200);
		NQE_INT_INPUT(FI_VOLZ_LEN, "Volume Z-Score Length", 50, 10, 1000);
		NQE_FLT_INPUT(FI_ABS_Z, "Absorption: Volume Z >=", 2.0, 0.5, 10.0);
		NQE_FLT_INPUT(FI_ABS_RANGE, "Absorption: Max Range (ATR)", 0.6, 0.1, 3.0);
		NQE_FLT_INPUT(FI_ABS_FRAC, "Absorption: Zone Fraction Of Range", 0.25, 0.05, 1.0);
		NQE_INT_INPUT(FI_EXH_RUN, "Exhaustion: Run Bars", 3, 1, 50);
		NQE_FLT_INPUT(FI_EXH_PCT, "Exhaustion: Extreme Volume % Of Bar Max", 15.0, 1.0, 100.0);
		NQE_FLT_INPUT(FI_IMB_RATIO, "Imbalance: Diagonal Ratio %", 300.0, 150.0, 2000.0);
		NQE_INT_INPUT(FI_IMB_MINVOL, "Imbalance: Min Volume Per Level", 10, 1, 100000);
		NQE_INT_INPUT(FI_IMB_STACK, "Imbalance: Stacked Levels >=", 3, 2, 20);
		NQE_FLT_INPUT(FI_LT_PCT, "Large Trade: Rolling Percentile", 99.0, 80.0, 99.99);
		NQE_INT_INPUT(FI_LT_MIN, "Large Trade: Min Size", 20, 1, 100000);
		NQE_INT_INPUT(FI_LT_LOOK, "Large Trade: Lookback Trades", 2000, 100, 50000);
		NQE_INT_INPUT(FI_LT_MAX, "Large Trade: Max Bubbles Drawn", 100, 0, 500);
		NQE_INT_INPUT(FI_TRAP_LOOK, "Trapped: Breakout Lookback Bars", 20, 3, 200);
		NQE_INT_INPUT(FI_TRAP_K, "Trapped: Reversal Within K Bars", 3, 1, 20);
		NQE_FLT_INPUT(FI_TRAP_DELTA, "Trapped: Min Breakout Delta %", 20.0, 0.0, 100.0);
		NQE_FLT_INPUT(FI_DIV_ATR, "Divergence: Min Swing Distance (ATR)", 0.5, 0.0, 10.0);
		NQE_INT_INPUT(FI_DECAY, "Event Decay Bars (feature half-life)", 8, 1, 100);
		NQE_INT_INPUT(FI_MAX_ZONES, "Max Active Zones Drawn", 30, 1, 200);
		NQE_YESNO_INPUT(FI_D_CVD, "Draw: CVD", 1);
		NQE_YESNO_INPUT(FI_D_ABS, "Draw: Absorption Zones", 1);
		NQE_YESNO_INPUT(FI_D_IMB, "Draw: Imbalance Zones", 1);
		NQE_YESNO_INPUT(FI_D_BUBBLE, "Draw: Large Trade Bubbles", 1);
		NQE_YESNO_INPUT(FI_D_DIV, "Draw: Divergence Markers", 1);
		NQE_YESNO_INPUT(FI_D_TRAP, "Draw: Trapped Trader Markers", 1);
		NQE_YESNO_INPUT(FI_D_EXH, "Draw: Exhaustion Markers", 1);
		NQE_COLOR_INPUT(FI_C_CVD_UP, "Color: CVD Up", 0, 200, 120);
		NQE_COLOR_INPUT(FI_C_CVD_DN, "Color: CVD Down", 220, 70, 70);
		NQE_COLOR_INPUT(FI_C_ABS_BULL, "Color: Absorption (buyers absorbed selling)", 0, 180, 110);
		NQE_COLOR_INPUT(FI_C_ABS_BEAR, "Color: Absorption (sellers absorbed buying)", 200, 60, 60);
		NQE_COLOR_INPUT(FI_C_IMB_BUY, "Color: Stacked Buy Imbalance", 40, 200, 90);
		NQE_COLOR_INPUT(FI_C_IMB_SELL, "Color: Stacked Sell Imbalance", 210, 50, 80);
		NQE_COLOR_INPUT(FI_C_BUB_BUY, "Color: Large Buy Bubble", 60, 230, 120);
		NQE_COLOR_INPUT(FI_C_BUB_SELL, "Color: Large Sell Bubble", 240, 80, 80);
		NQE_COLOR_INPUT(FI_C_DIV, "Color: CVD Divergence", 255, 180, 0);
		NQE_COLOR_INPUT(FI_C_TRAP, "Color: Trapped Traders", 255, 100, 255);
		NQE_COLOR_INPUT(FI_C_EXH, "Color: Exhaustion", 200, 200, 90);
		return;
	}
	if (sc.LastCallToFunction) { Release(sc, E_FLOW); return; }
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	ChartState& S = Acquire(sc);
	S.studyPresent[E_FLOW] = true;
	if (sc.MaintainVolumeAtPriceData == 0) { sc.MaintainVolumeAtPriceData = 1; sc.FlagToReloadChartData = 1; }
	FlowParams fp{};
	fp.cvdReset = sc.Input[FI_CVD_RESET].GetIndex(); fp.cvdSlopeBars = sc.Input[FI_CVD_SLOPE].GetInt(); fp.volZLength = sc.Input[FI_VOLZ_LEN].GetInt();
	fp.absorbVolZ = sc.Input[FI_ABS_Z].GetFloat(); fp.absorbMaxRangeAtr = sc.Input[FI_ABS_RANGE].GetFloat(); fp.absorbZoneFrac = sc.Input[FI_ABS_FRAC].GetFloat();
	fp.exhaustRunBars = sc.Input[FI_EXH_RUN].GetInt(); fp.exhaustExtremePct = sc.Input[FI_EXH_PCT].GetFloat();
	fp.imbRatioPct = sc.Input[FI_IMB_RATIO].GetFloat(); fp.imbMinVolume = sc.Input[FI_IMB_MINVOL].GetInt(); fp.imbStackLevels = sc.Input[FI_IMB_STACK].GetInt();
	fp.largePercentile = sc.Input[FI_LT_PCT].GetFloat(); fp.largeMinSize = sc.Input[FI_LT_MIN].GetInt(); fp.largeLookback = sc.Input[FI_LT_LOOK].GetInt(); fp.maxBubbles = sc.Input[FI_LT_MAX].GetInt();
	fp.trapLookback = sc.Input[FI_TRAP_LOOK].GetInt(); fp.trapReversalBars = sc.Input[FI_TRAP_K].GetInt(); fp.trapMinDeltaPct = sc.Input[FI_TRAP_DELTA].GetFloat();
	fp.divMinAtr = sc.Input[FI_DIV_ATR].GetFloat(); fp.eventDecayBars = sc.Input[FI_DECAY].GetInt(); fp.maxActiveZones = sc.Input[FI_MAX_ZONES].GetInt();
	SetParams(S, E_FLOW, S.params.flow, fp);
	CheckDataStamp(sc, S);
	EnsureFlow(sc, S);

	sc.Subgraph[FS_CVD].DrawStyle = sc.Input[FI_D_CVD].GetYesNo() ? DRAWSTYLE_LINE : DRAWSTYLE_IGNORE;
	int& gen = sc.GetPersistentInt(1);
	int start = Min(sc.UpdateStartIndex, S.flow.dirtyFrom); S.flow.dirtyFrom = INT_MAX;
	if (gen != S.flow.generation) { start = 0; gen = S.flow.generation; }
	if (start < 0) start = 0;
	const FlowState& F = S.flow;
	const uint32_t cUp = sc.Input[FI_C_CVD_UP].GetColor(), cDn = sc.Input[FI_C_CVD_DN].GetColor();
	for (int i = start; i < sc.ArraySize; ++i)
	{
		sc.Subgraph[FS_CVD][i] = F.cvd[i]; sc.Subgraph[FS_CVD].DataColor[i] = (i > 0 && F.cvd[i] < F.cvd[i - 1]) ? cDn : cUp;
		sc.Subgraph[FS_DELTA][i] = F.delta[i]; sc.Subgraph[FS_DELTA_PCT][i] = F.deltaPct[i]; sc.Subgraph[FS_VOLZ][i] = F.volZ[i]; sc.Subgraph[FS_CVDZ][i] = F.cvdZ[i];
		sc.Subgraph[FS_F_ABS][i] = F.fAbsorb[i]; sc.Subgraph[FS_F_EXH][i] = F.fExhaust[i]; sc.Subgraph[FS_F_IMB][i] = F.fImb[i]; sc.Subgraph[FS_F_TRAP][i] = F.fTrapped[i];
		sc.Subgraph[FS_F_DIV][i] = F.fCvdDiv[i]; sc.Subgraph[FS_F_LARGE][i] = F.fLarge[i];
	}

	// ---- drawings on the price chart (region 0) owned by this study ----
	FlowState& FW = S.flow;
	for (size_t k = 0; k < FW.deadLines.size(); ++k) { int ln = FW.deadLines[k]; DeleteDrawing(sc, ln); }
	FW.deadLines.clear();
	const int last = sc.ArraySize - 1;
	const int maxZones = Max(1, sc.Input[FI_MAX_ZONES].GetInt());
	struct ZoneDrawer
	{
		static void Draw(SCStudyInterfaceRef sc, std::vector<Zone>& zones, bool on, int last, int maxZones, uint32_t cBull, uint32_t cBear, const char* bullTxt, const char* bearTxt)
		{
			int drawn = 0;
			for (int z = static_cast<int>(zones.size()) - 1; z >= 0; --z)
			{
				Zone& Z = zones[z];
				if (!on) { DeleteDrawing(sc, Z.lineNumber); continue; }
				if (!Z.active)
				{
					if (Z.deadIdx >= 0 && last - Z.deadIdx > 3) DeleteDrawing(sc, Z.lineNumber);
					else if (Z.lineNumber) DrawRect(sc, Z.lineNumber, Z.bornIdx, Z.deadIdx, Z.top, Z.bottom, Z.dir > 0 ? cBull : cBear, 88);
					continue;
				}
				if (drawn++ >= maxZones) { DeleteDrawing(sc, Z.lineNumber); continue; }
				DrawRect(sc, Z.lineNumber, Z.bornIdx, last, Z.top, Z.bottom, Z.dir > 0 ? cBull : cBear, 70, Z.dir > 0 ? bullTxt : bearTxt);
			}
		}
	};
	ZoneDrawer::Draw(sc, FW.absorbZones, sc.Input[FI_D_ABS].GetYesNo() != 0, last, maxZones, sc.Input[FI_C_ABS_BULL].GetColor(), sc.Input[FI_C_ABS_BEAR].GetColor(), "absorption", "absorption");
	ZoneDrawer::Draw(sc, FW.imbZones, sc.Input[FI_D_IMB].GetYesNo() != 0, last, maxZones, sc.Input[FI_C_IMB_BUY].GetColor(), sc.Input[FI_C_IMB_SELL].GetColor(), "stacked buy imb", "stacked sell imb");

	// bubbles: size scaled by rank within the visible set
	{
		const bool on = sc.Input[FI_D_BUBBLE].GetYesNo() != 0;
		const int maxB = Max(0, sc.Input[FI_LT_MAX].GetInt());
		double maxSize = 1; for (size_t q = 0; q < FW.bubbles.size(); ++q) maxSize = Max(maxSize, FW.bubbles[q].size);
		int drawn = 0;
		for (int q = static_cast<int>(FW.bubbles.size()) - 1; q >= 0; --q)
		{
			Bubble& bb = FW.bubbles[q];
			if (!on || drawn >= maxB) { DeleteDrawing(sc, bb.lineNumber); continue; }
			++drawn;
			const int sz = 4 + static_cast<int>(12.0 * sqrt(bb.size / maxSize));
			const uint32_t col = bb.dir > 0 ? sc.Input[FI_C_BUB_BUY].GetColor() : sc.Input[FI_C_BUB_SELL].GetColor();
			// live bubbles on the forming bar can still grow; everything else is drawn once
			if (bb.lineNumber == 0 || (bb.live && bb.idx == last)) DrawMarker(sc, bb.lineNumber, bb.idx, bb.price, bb.live ? MARKER_POINT : MARKER_SQUARE, sz, col, bb.live ? sz : 1);
		}
	}

	// event markers (divergence = 1, trapped = 2, exhaustion = 3)
	{
		const bool dDiv = sc.Input[FI_D_DIV].GetYesNo() != 0, dTrap = sc.Input[FI_D_TRAP].GetYesNo() != 0, dExh = sc.Input[FI_D_EXH].GetYesNo() != 0;
		for (size_t q = 0; q < FW.markers.size(); ++q)
		{
			FlowState::Marker& m = FW.markers[q];
			const bool on = (m.kind == 1 && dDiv) || (m.kind == 2 && dTrap) || (m.kind == 3 && dExh);
			if (!on) { DeleteDrawing(sc, m.lineNumber); continue; }
			if (m.lineNumber != 0) continue;
			const uint32_t col = m.kind == 1 ? sc.Input[FI_C_DIV].GetColor() : (m.kind == 2 ? sc.Input[FI_C_TRAP].GetColor() : sc.Input[FI_C_EXH].GetColor());
			const int type = m.kind == 1 ? (m.dir > 0 ? MARKER_TRIANGLEUP : MARKER_TRIANGLEDOWN) : (m.kind == 2 ? MARKER_X : MARKER_DIAMOND);
			DrawMarker(sc, m.lineNumber, m.idx, m.price, type, 7, col, 2);
		}
	}
}

// --- 4. Regime + MTF ----------------------------------------------------------
enum RegimeInput { RI_ER_LEN = 0, RI_ER_TREND, RI_ER_BAL, RI_ATR_FAST, RI_ATR_SLOW, RI_CHOP, RI_IB_DAYS, RI_INSIDE, RI_HYST, RI_MTF_EMA, RI_MTF_SWING, RI_SHADE, RI_C_UP, RI_C_DN, RI_C_BAL, RI_C_CHOP, RI_COUNT };
enum RegimeSubgraph { RS_BG = 0, RS_REGIME, RS_ER, RS_ATR_RATIO, RS_TREND, RS_MTF1, RS_MTF5, RS_MTF15, RS_MTF60, RS_MTF_BIAS, RS_COUNT };

SCSFExport scsf_NQEdge_Regime(SCStudyInterfaceRef sc)
{
	if (sc.SetDefaults)
	{
		sc.GraphName = "NQ Edge: Regime + MTF Bias";
		sc.StudyDescription = "Classifies every closed bar as Trend Up / Trend Down / Balance / Volatile Chop (with hysteresis) and computes 1m/5m/15m/60m trend bias. Shades the chart background by regime.";
		sc.GraphRegion = 0; sc.AutoLoop = 0; sc.CalculationPrecedence = LOW_PREC_LEVEL; sc.ValueFormat = 2; sc.ScaleRangeType = SCALE_INDEPENDENT; sc.DrawZeros = 0; sc.DrawStudyUnderneathMainPriceGraph = 1;
		const char* names[RS_COUNT] = { "Regime Shade", "Regime", "Efficiency Ratio", "ATR Ratio", "f.regimeTrend", "MTF 1m", "MTF 5m", "MTF 15m", "MTF 60m", "f.mtfBias" };
		for (int k = 0; k < RS_COUNT; ++k) { sc.Subgraph[k].Name = names[k]; sc.Subgraph[k].DrawStyle = DRAWSTYLE_IGNORE; sc.Subgraph[k].DrawZeros = 0; }
		sc.Subgraph[RS_BG].DrawStyle = DRAWSTYLE_BACKGROUND; sc.Subgraph[RS_BG].PrimaryColor = RGB(20, 40, 20);
		NQE_INT_INPUT(RI_ER_LEN, "Efficiency Ratio Length", 20, 5, 500);
		NQE_FLT_INPUT(RI_ER_TREND, "ER Trend Threshold", 0.35, 0.05, 1.0);
		NQE_FLT_INPUT(RI_ER_BAL, "ER Balance Threshold", 0.20, 0.0, 1.0);
		NQE_INT_INPUT(RI_ATR_FAST, "ATR Fast Length", 7, 2, 100);
		NQE_INT_INPUT(RI_ATR_SLOW, "ATR Slow Length", 50, 5, 1000);
		NQE_FLT_INPUT(RI_CHOP, "Chop: ATR Expansion Ratio >=", 1.3, 1.0, 5.0);
		NQE_INT_INPUT(RI_IB_DAYS, "IB Range Average Days", 20, 1, 100);
		NQE_INT_INPUT(RI_INSIDE, "Inside-Value Lookback Bars", 30, 5, 500);
		NQE_INT_INPUT(RI_HYST, "Hysteresis Bars", 3, 1, 50);
		NQE_INT_INPUT(RI_MTF_EMA, "MTF EMA Length", 20, 2, 200);
		NQE_INT_INPUT(RI_MTF_SWING, "MTF Swing Strength", 3, 1, 20);
		NQE_YESNO_INPUT(RI_SHADE, "Shade Background By Regime", 1);
		NQE_COLOR_INPUT(RI_C_UP, "Color: Trend Up Shade", 18, 40, 24);
		NQE_COLOR_INPUT(RI_C_DN, "Color: Trend Down Shade", 44, 20, 22);
		NQE_COLOR_INPUT(RI_C_BAL, "Color: Balance Shade", 26, 26, 34);
		NQE_COLOR_INPUT(RI_C_CHOP, "Color: Chop Shade", 40, 34, 18);
		return;
	}
	if (sc.LastCallToFunction) { Release(sc, E_REGIME); return; }
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	ChartState& S = Acquire(sc);
	S.studyPresent[E_REGIME] = true;
	RegimeParams rp{};
	rp.erLength = sc.Input[RI_ER_LEN].GetInt(); rp.erTrend = sc.Input[RI_ER_TREND].GetFloat(); rp.erBalance = sc.Input[RI_ER_BAL].GetFloat();
	rp.atrFast = sc.Input[RI_ATR_FAST].GetInt(); rp.atrSlow = sc.Input[RI_ATR_SLOW].GetInt(); rp.chopExpansion = sc.Input[RI_CHOP].GetFloat();
	rp.ibAvgDays = sc.Input[RI_IB_DAYS].GetInt(); rp.insideValueBars = sc.Input[RI_INSIDE].GetInt(); rp.hysteresisBars = sc.Input[RI_HYST].GetInt();
	rp.mtfEmaLength = sc.Input[RI_MTF_EMA].GetInt(); rp.mtfSwing = sc.Input[RI_MTF_SWING].GetInt();
	SetParams(S, E_REGIME, S.params.regime, rp);
	CheckDataStamp(sc, S);
	EnsureRegime(sc, S);

	const bool shade = sc.Input[RI_SHADE].GetYesNo() != 0;
	sc.Subgraph[RS_BG].DrawStyle = shade ? DRAWSTYLE_BACKGROUND : DRAWSTYLE_IGNORE;
	const uint32_t cols[5] = { 0, sc.Input[RI_C_UP].GetColor(), sc.Input[RI_C_DN].GetColor(), sc.Input[RI_C_BAL].GetColor(), sc.Input[RI_C_CHOP].GetColor() };
	int& gen = sc.GetPersistentInt(1);
	int start = Min(sc.UpdateStartIndex, S.regime.dirtyFrom); S.regime.dirtyFrom = INT_MAX;
	if (gen != S.regime.generation) { start = 0; gen = S.regime.generation; }
	if (start < 0) start = 0;
	const RegimeState& R = S.regime;
	for (int i = start; i < sc.ArraySize; ++i)
	{
		const int r = Clamp(static_cast<int>(R.regime[i]), 0, 4);
		sc.Subgraph[RS_BG][i] = (shade && r != 0) ? 1.0f : 0.0f; sc.Subgraph[RS_BG].DataColor[i] = cols[r];
		sc.Subgraph[RS_REGIME][i] = static_cast<float>(r); sc.Subgraph[RS_ER][i] = R.er[i]; sc.Subgraph[RS_ATR_RATIO][i] = R.atrRatio[i]; sc.Subgraph[RS_TREND][i] = R.regimeTrend[i];
		sc.Subgraph[RS_MTF1][i] = R.mtf1[i]; sc.Subgraph[RS_MTF5][i] = R.mtf5[i]; sc.Subgraph[RS_MTF15][i] = R.mtf15[i]; sc.Subgraph[RS_MTF60][i] = R.mtf60[i]; sc.Subgraph[RS_MTF_BIAS][i] = R.mtfBias[i];
	}
}

// --- 5. Intermarket -----------------------------------------------------------
enum InterInput { II_YM = 0, II_ES, II_RTY, II_TICK, II_MEGA1, II_MEGA2, II_MEGA3, II_MEGA4, II_MEGA5, II_MEGA6, II_RS_LOOK, II_RS_Z, II_TICK_EXT, II_TICK_STRONG, II_TICK_LOOK, II_TICK_EMA, II_MEGA_EMA, II_C_RS, II_C_BREADTH, II_COUNT };
enum InterSubgraph { IS_RS_YM = 0, IS_RS_ES, IS_RS_RTY, IS_RS_INDEX, IS_SMT, IS_TICK_CUM, IS_TICK_EXT, IS_TICK_DIV, IS_MEGA, IS_COMPOSITE, IS_COUNT };

SCSFExport scsf_NQEdge_Intermarket(SCStudyInterfaceRef sc)
{
	if (sc.SetDefaults)
	{
		sc.GraphName = "NQ Edge: Intermarket Engine";
		sc.StudyDescription = "Relative strength vs YM/ES/RTY, SMT divergence, NYSE TICK (cumulative, extremes, divergence), mega-cap leadership breadth. Every chart number is optional (0 = off).";
		sc.GraphRegion = 2; sc.AutoLoop = 0; sc.CalculationPrecedence = LOW_PREC_LEVEL; sc.ValueFormat = 2; sc.DrawZeros = 1;
		const char* names[IS_COUNT] = { "RS vs YM", "RS vs ES", "RS vs RTY", "f.rsIndex", "f.smt", "f.tickCum", "f.tickExt", "f.tickDiv", "f.megaCap", "Intermarket Composite" };
		for (int k = 0; k < IS_COUNT; ++k) { sc.Subgraph[k].Name = names[k]; sc.Subgraph[k].DrawStyle = DRAWSTYLE_IGNORE; sc.Subgraph[k].DrawZeros = 1; }
		sc.Subgraph[IS_RS_INDEX].DrawStyle = DRAWSTYLE_LINE; sc.Subgraph[IS_RS_INDEX].PrimaryColor = RGB(120, 180, 255);
		sc.Subgraph[IS_MEGA].DrawStyle = DRAWSTYLE_LINE; sc.Subgraph[IS_MEGA].PrimaryColor = RGB(255, 200, 80);
		sc.Subgraph[IS_COMPOSITE].DrawStyle = DRAWSTYLE_BAR; sc.Subgraph[IS_COMPOSITE].PrimaryColor = RGB(90, 90, 90);
		sc.Input[II_YM].Name = "Chart Number: YM (0 = off)"; sc.Input[II_YM].SetChartNumber(0);
		sc.Input[II_ES].Name = "Chart Number: ES (0 = off)"; sc.Input[II_ES].SetChartNumber(0);
		sc.Input[II_RTY].Name = "Chart Number: RTY (0 = off)"; sc.Input[II_RTY].SetChartNumber(0);
		sc.Input[II_TICK].Name = "Chart Number: NYSE TICK (0 = off)"; sc.Input[II_TICK].SetChartNumber(0);
		const char* mn[6] = { "Chart Number: Mega Cap 1 (e.g. AAPL)", "Chart Number: Mega Cap 2 (e.g. AMZN)", "Chart Number: Mega Cap 3 (e.g. NVDA)", "Chart Number: Mega Cap 4 (e.g. MSFT)", "Chart Number: Mega Cap 5 (e.g. META)", "Chart Number: Mega Cap 6 (e.g. GOOGL)" };
		for (int k = 0; k < 6; ++k) { sc.Input[II_MEGA1 + k].Name = mn[k]; sc.Input[II_MEGA1 + k].SetChartNumber(0); }
		NQE_INT_INPUT(II_RS_LOOK, "RS: Return Lookback Bars", 20, 2, 500);
		NQE_INT_INPUT(II_RS_Z, "RS: Z-Score Length", 100, 10, 2000);
		NQE_FLT_INPUT(II_TICK_EXT, "TICK: Extreme Level", 800.0, 100.0, 3000.0);
		NQE_FLT_INPUT(II_TICK_STRONG, "TICK: Strong Extreme Level", 1000.0, 100.0, 3000.0);
		NQE_INT_INPUT(II_TICK_LOOK, "TICK: Extremes Lookback Bars", 10, 1, 200);
		NQE_INT_INPUT(II_TICK_EMA, "TICK: Trend EMA Length", 10, 2, 200);
		NQE_INT_INPUT(II_MEGA_EMA, "Mega Cap: Trend EMA Length", 20, 2, 200);
		NQE_COLOR_INPUT(II_C_RS, "Color: RS Line", 120, 180, 255);
		NQE_COLOR_INPUT(II_C_BREADTH, "Color: Leadership Breadth", 255, 200, 80);
		return;
	}
	if (sc.LastCallToFunction) { Release(sc, E_INTER); return; }
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	ChartState& S = Acquire(sc);
	S.studyPresent[E_INTER] = true;
	InterParams ip{};
	ip.chartYM = sc.Input[II_YM].GetChartNumber(); ip.chartES = sc.Input[II_ES].GetChartNumber(); ip.chartRTY = sc.Input[II_RTY].GetChartNumber(); ip.chartTICK = sc.Input[II_TICK].GetChartNumber();
	for (int k = 0; k < 6; ++k) ip.chartMega[k] = sc.Input[II_MEGA1 + k].GetChartNumber();
	ip.rsLookback = sc.Input[II_RS_LOOK].GetInt(); ip.rsZLength = sc.Input[II_RS_Z].GetInt(); ip.tickExtreme = sc.Input[II_TICK_EXT].GetFloat(); ip.tickStrong = sc.Input[II_TICK_STRONG].GetFloat();
	ip.tickLookback = sc.Input[II_TICK_LOOK].GetInt(); ip.tickEma = sc.Input[II_TICK_EMA].GetInt(); ip.megaEma = sc.Input[II_MEGA_EMA].GetInt();
	SetParams(S, E_INTER, S.params.inter, ip);
	CheckDataStamp(sc, S);
	EnsureInter(sc, S);
	sc.Subgraph[IS_RS_INDEX].PrimaryColor = sc.Input[II_C_RS].GetColor(); sc.Subgraph[IS_MEGA].PrimaryColor = sc.Input[II_C_BREADTH].GetColor();
	int& gen = sc.GetPersistentInt(1);
	int start = sc.UpdateStartIndex;
	if (gen != S.inter.generation) { start = 0; gen = S.inter.generation; }
	const InterState& I = S.inter;
	for (int i = start; i < sc.ArraySize; ++i)
	{
		sc.Subgraph[IS_RS_YM][i] = I.rsYM[i]; sc.Subgraph[IS_RS_ES][i] = I.rsES[i]; sc.Subgraph[IS_RS_RTY][i] = I.rsRTY[i]; sc.Subgraph[IS_RS_INDEX][i] = I.rsIndex[i];
		sc.Subgraph[IS_SMT][i] = I.smt[i]; sc.Subgraph[IS_TICK_CUM][i] = I.tickCum[i]; sc.Subgraph[IS_TICK_EXT][i] = I.tickExt[i]; sc.Subgraph[IS_TICK_DIV][i] = I.tickDiv[i];
		sc.Subgraph[IS_MEGA][i] = I.megaCap[i]; sc.Subgraph[IS_COMPOSITE][i] = I.composite[i];
	}
}

// --- 6. DCS -------------------------------------------------------------------
enum DcsInput
{
	DI_FILE = 0, DI_RELOAD, DI_THR, DI_FADE, DI_SMOOTH, DI_STRONG, DI_WEAK, DI_S1, DI_S2, DI_S3, DI_S4, DI_S5, DI_MTF, DI_TOL, DI_STOPBUF, DI_MINRR, DI_MINTGT, DI_MAXSIG, DI_TGTBARS, DI_ALERTS, DI_SOUND,
	DI_C_GREEN, DI_C_RED, DI_C_GRAY, DI_C_SMOOTH, DI_C_THR, DI_C_LONG, DI_C_SHORT, DI_C_ENTRY, DI_C_STOP, DI_C_TARGET, DI_C_LABEL, DI_COUNT
};
enum DcsSubgraph { DS_DCS = 0, DS_SMOOTH, DS_THR_UP, DS_THR_DN, DS_SIGNAL, DS_BIAS, DS_COUNT };

SCSFExport scsf_NQEdge_DCS(SCStudyInterfaceRef sc)
{
	if (sc.SetDefaults)
	{
		sc.GraphName = "NQ Edge: Directional Conviction Score";
		sc.StudyDescription = "Fuses all engine features into a regime-gated score (-100..+100), detects the five trade setups with structural stops and liquidity targets, draws and alerts them.";
		sc.GraphRegion = 3; sc.AutoLoop = 0; sc.CalculationPrecedence = VERY_LOW_PREC_LEVEL; sc.ValueFormat = 0; sc.DrawZeros = 1;
		const char* names[DS_COUNT] = { "DCS", "DCS Smoothed", "+Threshold", "-Threshold", "Signal", "Bias" };
		for (int k = 0; k < DS_COUNT; ++k) { sc.Subgraph[k].Name = names[k]; sc.Subgraph[k].DrawZeros = 1; }
		sc.Subgraph[DS_DCS].DrawStyle = DRAWSTYLE_BAR; sc.Subgraph[DS_DCS].PrimaryColor = RGB(128, 128, 128); sc.Subgraph[DS_DCS].LineWidth = 2;
		sc.Subgraph[DS_SMOOTH].DrawStyle = DRAWSTYLE_LINE; sc.Subgraph[DS_SMOOTH].PrimaryColor = RGB(255, 255, 255); sc.Subgraph[DS_SMOOTH].LineWidth = 1;
		sc.Subgraph[DS_THR_UP].DrawStyle = DRAWSTYLE_DASH; sc.Subgraph[DS_THR_UP].PrimaryColor = RGB(90, 90, 90);
		sc.Subgraph[DS_THR_DN].DrawStyle = DRAWSTYLE_DASH; sc.Subgraph[DS_THR_DN].PrimaryColor = RGB(90, 90, 90);
		sc.Subgraph[DS_SIGNAL].DrawStyle = DRAWSTYLE_IGNORE; sc.Subgraph[DS_BIAS].DrawStyle = DRAWSTYLE_IGNORE;
		sc.Input[DI_FILE].Name = "Weights File Name (in Data folder)"; sc.Input[DI_FILE].SetString("NQEdge_weights.txt");
		NQE_INT_INPUT(DI_RELOAD, "Weights Hot-Reload Check Seconds", 5, 1, 600);
		NQE_FLT_INPUT(DI_THR, "Signal Threshold (|DCS| >=)", 40.0, 0.0, 100.0);
		NQE_FLT_INPUT(DI_FADE, "Fade Threshold In Balance (|DCS| >=)", 20.0, 0.0, 100.0);
		NQE_INT_INPUT(DI_SMOOTH, "Smoothing Length (EMA)", 5, 1, 50);
		NQE_FLT_INPUT(DI_STRONG, "Bar State: Strong Threshold", 60.0, 0.0, 100.0);
		NQE_FLT_INPUT(DI_WEAK, "Bar State: Weak Threshold", 25.0, 0.0, 100.0);
		NQE_YESNO_INPUT(DI_S1, "Setup: Trend Pullback Continuation", 1);
		NQE_YESNO_INPUT(DI_S2, "Setup: Value-Edge Rejection (Balance)", 1);
		NQE_YESNO_INPUT(DI_S3, "Setup: Failed Breakout / Trapped Traders", 1);
		NQE_YESNO_INPUT(DI_S4, "Setup: Break-and-Acceptance", 1);
		NQE_YESNO_INPUT(DI_S5, "Setup: SMT/CVD Divergence At Liquidity", 1);
		NQE_YESNO_INPUT(DI_MTF, "Require MTF Alignment For Trend Setups", 1);
		NQE_FLT_INPUT(DI_TOL, "Location Tolerance (ATR)", 0.3, 0.05, 3.0);
		NQE_FLT_INPUT(DI_STOPBUF, "Stop Buffer Beyond Structure (ATR)", 0.5, 0.0, 3.0);
		NQE_FLT_INPUT(DI_MINRR, "Min Reward:Risk To T1", 1.0, 0.1, 10.0);
		NQE_FLT_INPUT(DI_MINTGT, "Min Target Distance (ATR)", 0.5, 0.1, 10.0);
		NQE_INT_INPUT(DI_MAXSIG, "Max Signals Drawn", 30, 1, 300);
		NQE_INT_INPUT(DI_TGTBARS, "Entry/Stop/Target Line Length (bars)", 20, 2, 200);
		NQE_YESNO_INPUT(DI_ALERTS, "Alerts Enabled", 1);
		sc.Input[DI_SOUND].Name = "Alert Sound Number"; sc.Input[DI_SOUND].SetAlertSoundNumber(1);
		NQE_COLOR_INPUT(DI_C_GREEN, "Color: Deep Green (DCS +100)", 0, 220, 110);
		NQE_COLOR_INPUT(DI_C_RED, "Color: Deep Red (DCS -100)", 230, 60, 60);
		NQE_COLOR_INPUT(DI_C_GRAY, "Color: Neutral Gray (DCS 0)", 110, 110, 110);
		NQE_COLOR_INPUT(DI_C_SMOOTH, "Color: Smoothed Line", 255, 255, 255);
		NQE_COLOR_INPUT(DI_C_THR, "Color: Threshold Lines", 90, 90, 90);
		NQE_COLOR_INPUT(DI_C_LONG, "Color: Long Signal", 0, 230, 120);
		NQE_COLOR_INPUT(DI_C_SHORT, "Color: Short Signal", 240, 70, 70);
		NQE_COLOR_INPUT(DI_C_ENTRY, "Color: Entry Line", 255, 255, 255);
		NQE_COLOR_INPUT(DI_C_STOP, "Color: Stop Line", 230, 60, 60);
		NQE_COLOR_INPUT(DI_C_TARGET, "Color: Target Lines", 0, 200, 255);
		NQE_COLOR_INPUT(DI_C_LABEL, "Color: Signal Label", 255, 255, 255);
		return;
	}
	if (sc.LastCallToFunction) { Release(sc, E_DCS); return; }
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	ChartState& S = Acquire(sc);
	S.studyPresent[E_DCS] = true;
	DcsParams dp{};
	strncpy_s(dp.weightsFile, sizeof(dp.weightsFile), sc.Input[DI_FILE].GetString(), _TRUNCATE);
	dp.hotReloadSec = sc.Input[DI_RELOAD].GetInt(); dp.signalThr = sc.Input[DI_THR].GetFloat(); dp.fadeThr = sc.Input[DI_FADE].GetFloat(); dp.smoothLen = sc.Input[DI_SMOOTH].GetInt();
	dp.strongThr = sc.Input[DI_STRONG].GetFloat(); dp.weakThr = sc.Input[DI_WEAK].GetFloat();
	for (int k = 1; k < SETUP_COUNT; ++k) dp.setupOn[k] = sc.Input[DI_S1 + k - 1].GetYesNo();
	dp.requireMtf = sc.Input[DI_MTF].GetYesNo(); dp.levelTolAtr = sc.Input[DI_TOL].GetFloat(); dp.stopBufferAtr = sc.Input[DI_STOPBUF].GetFloat(); dp.minRR = sc.Input[DI_MINRR].GetFloat();
	dp.minTargetAtr = sc.Input[DI_MINTGT].GetFloat(); dp.maxSignalsDrawn = sc.Input[DI_MAXSIG].GetInt(); dp.targetLineBars = sc.Input[DI_TGTBARS].GetInt();
	dp.alertsOn = sc.Input[DI_ALERTS].GetYesNo(); dp.alertSound = sc.Input[DI_SOUND].GetAlertSoundNumber();
	SetParams(S, E_DCS, S.params.dcs, dp);
	CheckDataStamp(sc, S);
	EnsureDcs(sc, S);

	sc.Subgraph[DS_SMOOTH].PrimaryColor = sc.Input[DI_C_SMOOTH].GetColor(); sc.Subgraph[DS_THR_UP].PrimaryColor = sc.Subgraph[DS_THR_DN].PrimaryColor = sc.Input[DI_C_THR].GetColor();
	const uint32_t cG = sc.Input[DI_C_GREEN].GetColor(), cR = sc.Input[DI_C_RED].GetColor(), cN = sc.Input[DI_C_GRAY].GetColor();
	int& gen = sc.GetPersistentInt(1);
	int start = sc.UpdateStartIndex;
	if (gen != S.dcs.generation) { start = 0; gen = S.dcs.generation; }
	const DcsState& D = S.dcs;
	for (int i = start; i < sc.ArraySize; ++i)
	{
		const float v = D.dcs[i];
		sc.Subgraph[DS_DCS][i] = v; sc.Subgraph[DS_SMOOTH][i] = D.dcsSmooth[i];
		sc.Subgraph[DS_THR_UP][i] = D.weights.thrSignal; sc.Subgraph[DS_THR_DN][i] = -D.weights.thrSignal;
		const float t = Clamp(static_cast<float>(fabs(v)) / 100.0f, 0.0f, 1.0f);
		uint32_t c = v >= 0 ? sc.RGBInterpolate(cN, cG, t) : sc.RGBInterpolate(cN, cR, t);
		if (i == sc.ArraySize - 1) c = sc.RGBInterpolate(c, RGB(0, 0, 0), 0.5f);   // provisional (forming bar) drawn dim
		sc.Subgraph[DS_DCS].DataColor[i] = c;
		sc.Subgraph[DS_SIGNAL][i] = static_cast<float>(D.signalType[i]) * static_cast<float>(D.signalDir[i]);
		sc.Subgraph[DS_BIAS][i] = static_cast<float>(D.barState[i]);
	}
}

// --- 7. Validation ------------------------------------------------------------
enum ValInput { VLI_SLIP = 0, VLI_MAXBARS, VLI_STOPFIRST, VLI_MINSAMPLE, VLI_C_CURVE, VLI_C_MARK, VLI_COUNT };
enum ValSubgraph { VLS_CUMR = 0, VLS_RESULT, VLS_COUNT };

SCSFExport scsf_NQEdge_Validation(SCStudyInterfaceRef sc)
{
	if (sc.SetDefaults)
	{
		sc.GraphName = "NQ Edge: Signal Validation";
		sc.StudyDescription = "Replays every DCS signal forward on closed bars (1-tick slippage, stop-first) and reports per-setup count, win %, average R, profit factor, T2 %, MFE/MAE to the HUD.";
		sc.GraphRegion = 4; sc.AutoLoop = 0; sc.CalculationPrecedence = VERY_LOW_PREC_LEVEL; sc.ValueFormat = 2; sc.DrawZeros = 1;
		sc.Subgraph[VLS_CUMR].Name = "Cumulative R"; sc.Subgraph[VLS_CUMR].DrawStyle = DRAWSTYLE_LINE; sc.Subgraph[VLS_CUMR].PrimaryColor = RGB(0, 200, 255); sc.Subgraph[VLS_CUMR].LineWidth = 2;
		sc.Subgraph[VLS_RESULT].Name = "Signal Result (R)"; sc.Subgraph[VLS_RESULT].DrawStyle = DRAWSTYLE_POINT; sc.Subgraph[VLS_RESULT].PrimaryColor = RGB(255, 255, 255); sc.Subgraph[VLS_RESULT].LineWidth = 4; sc.Subgraph[VLS_RESULT].DrawZeros = 0;
		NQE_INT_INPUT(VLI_SLIP, "Slippage Ticks (entry and stop)", 1, 0, 20);
		NQE_INT_INPUT(VLI_MAXBARS, "Max Bars To Resolution", 120, 5, 2000);
		NQE_YESNO_INPUT(VLI_STOPFIRST, "Assume Stop First When Bar Hits Both", 1);
		NQE_INT_INPUT(VLI_MINSAMPLE, "Min Sample Size (warn below)", 30, 1, 1000);
		NQE_COLOR_INPUT(VLI_C_CURVE, "Color: Cumulative R", 0, 200, 255);
		NQE_COLOR_INPUT(VLI_C_MARK, "Color: Result Marker", 255, 255, 255);
		return;
	}
	if (sc.LastCallToFunction) { Release(sc, E_VAL); return; }
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	ChartState& S = Acquire(sc);
	S.studyPresent[E_VAL] = true;
	ValParams vp{}; vp.slippageTicks = sc.Input[VLI_SLIP].GetInt(); vp.maxBars = sc.Input[VLI_MAXBARS].GetInt(); vp.stopFirst = sc.Input[VLI_STOPFIRST].GetYesNo(); vp.minSample = sc.Input[VLI_MINSAMPLE].GetInt();
	SetParams(S, E_VAL, S.params.val, vp);
	CheckDataStamp(sc, S);
	EnsureVal(sc, S);
	sc.Subgraph[VLS_CUMR].PrimaryColor = sc.Input[VLI_C_CURVE].GetColor(); sc.Subgraph[VLS_RESULT].PrimaryColor = sc.Input[VLI_C_MARK].GetColor();
	int& gen = sc.GetPersistentInt(1);
	int start = sc.UpdateStartIndex;
	if (gen != S.val.generation) { start = 0; gen = S.val.generation; }
	for (int i = start; i < sc.ArraySize; ++i) { sc.Subgraph[VLS_CUMR][i] = S.val.cumR[i]; sc.Subgraph[VLS_RESULT][i] = 0; }
	for (size_t k = 0; k < S.dcs.signals.size(); ++k)
	{
		const Signal& g = S.dcs.signals[k];
		if (g.resolved != 0 && g.resIdx >= start && g.resIdx < sc.ArraySize) sc.Subgraph[VLS_RESULT][g.resIdx] = g.resultR;
	}
}

// --- 8. Feature logger --------------------------------------------------------
enum LogInput { LI_ENABLED = 0, LI_PREFIX, LI_REWRITE, LI_RTH, LI_COUNT };

SCSFExport scsf_NQEdge_FeatureLogger(SCStudyInterfaceRef sc)
{
	if (sc.SetDefaults)
	{
		sc.GraphName = "NQ Edge: Feature Logger";
		sc.StudyDescription = "Writes one CSV row per closed bar (OHLCV, every engine feature, DCS, regime, setup flags, forward returns at +5/+15/+30/+60 min, MFE/MAE) to the Data folder for the Python research loop.";
		sc.GraphRegion = 0; sc.AutoLoop = 0; sc.CalculationPrecedence = VERY_LOW_PREC_LEVEL; sc.DrawZeros = 0;
		sc.Subgraph[0].Name = "Rows Written"; sc.Subgraph[0].DrawStyle = DRAWSTYLE_IGNORE;
		NQE_YESNO_INPUT(LI_ENABLED, "Logging Enabled", 1);
		sc.Input[LI_PREFIX].Name = "File Name Prefix"; sc.Input[LI_PREFIX].SetString("NQEdge_features");
		NQE_YESNO_INPUT(LI_REWRITE, "Rewrite File On Full Recalculation", 1);
		NQE_YESNO_INPUT(LI_RTH, "Log RTH Bars Only", 0);
		return;
	}
	if (sc.LastCallToFunction) { Release(sc, E_LOG); return; }
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	ChartState& S = Acquire(sc);
	S.studyPresent[E_LOG] = true;
	LogParams lp{}; lp.enabled = sc.Input[LI_ENABLED].GetYesNo(); strncpy_s(lp.prefix, sizeof(lp.prefix), sc.Input[LI_PREFIX].GetString(), _TRUNCATE);
	lp.rewriteOnRecalc = sc.Input[LI_REWRITE].GetYesNo(); lp.rthOnly = sc.Input[LI_RTH].GetYesNo();
	SetParams(S, E_LOG, S.params.log, lp);
	CheckDataStamp(sc, S);
	EnsureLog(sc, S);
}

// --- 9. HUD + Bar Painter -----------------------------------------------------
SCSFExport scsf_NQEdge_HUD(SCStudyInterfaceRef sc)
{
	if (sc.SetDefaults)
	{
		sc.GraphName = "NQ Edge: HUD + Bar Painter";
		sc.StudyDescription = "Heads-up panel (bias, DCS, regime, MTF strip, intermarket, order flow, nearest levels, plain-English state, live stats, warnings) and DCS bar painting.";
		sc.GraphRegion = 0; sc.AutoLoop = 0; sc.CalculationPrecedence = VERY_LOW_PREC_LEVEL; sc.ValueFormat = VALUEFORMAT_INHERITED; sc.ScaleRangeType = SCALE_SAMEASREGION; sc.DrawZeros = 0;
		sc.Subgraph[0].Name = "DCS Bar Paint"; sc.Subgraph[0].DrawStyle = DRAWSTYLE_COLOR_BAR; sc.Subgraph[0].PrimaryColor = RGB(128, 128, 128); sc.Subgraph[0].DrawZeros = 0;
		sc.Input[HI_POSITION].Name = "HUD Position"; sc.Input[HI_POSITION].SetCustomInputStrings("Top Left;Top Right;Bottom Left;Bottom Right"); sc.Input[HI_POSITION].SetCustomInputIndex(0);
		sc.Input[HI_PRESET].Name = "Preset"; sc.Input[HI_PRESET].SetCustomInputStrings("Minimal;Full"); sc.Input[HI_PRESET].SetCustomInputIndex(1);
		NQE_INT_INPUT(HI_FONT, "Font Size (pt)", 11, 7, 24);
		NQE_INT_INPUT(HI_WIDTH, "Panel Width (px)", 360, 200, 1200);
		NQE_INT_INPUT(HI_OPACITY, "Panel Opacity %", 80, 0, 100);
		NQE_YESNO_INPUT(HI_PAINT, "Paint Price Bars By DCS State", 1);
		NQE_YESNO_INPUT(HI_SHOW_WARN, "Show Warnings", 1);
		NQE_YESNO_INPUT(HI_SHOW_STATS, "Show Signal Stats", 1);
		NQE_YESNO_INPUT(HI_SHOW_INTER, "Show Intermarket Row", 1);
		NQE_YESNO_INPUT(HI_SHOW_FLOW, "Show Order Flow Row", 1);
		NQE_YESNO_INPUT(HI_SHOW_LEVELS, "Show Nearest Levels Row", 1);
		NQE_COLOR_INPUT(HI_C_BG, "Color: Panel Background", 12, 14, 18);
		NQE_COLOR_INPUT(HI_C_TEXT, "Color: Text", 220, 220, 220);
		NQE_COLOR_INPUT(HI_C_BULL, "Color: Bull", 0, 220, 110);
		NQE_COLOR_INPUT(HI_C_BEAR, "Color: Bear", 240, 70, 70);
		NQE_COLOR_INPUT(HI_C_NEUTRAL, "Color: Neutral", 150, 150, 150);
		NQE_COLOR_INPUT(HI_C_LEVEL, "Color: Levels (cyan)", 0, 200, 255);
		NQE_COLOR_INPUT(HI_C_VWAP, "Color: VWAP/State (gold)", 255, 215, 0);
		NQE_COLOR_INPUT(HI_C_WARN, "Color: Warning", 255, 170, 0);
		NQE_COLOR_INPUT(HI_C_STRONG_BULL, "Bar Color: Strong Bull", 0, 230, 120);
		NQE_COLOR_INPUT(HI_C_WEAK_BULL, "Bar Color: Weak Bull", 0, 140, 80);
		NQE_COLOR_INPUT(HI_C_NEUTRAL_BAR, "Bar Color: Neutral", 120, 120, 120);
		NQE_COLOR_INPUT(HI_C_WEAK_BEAR, "Bar Color: Weak Bear", 150, 60, 60);
		NQE_COLOR_INPUT(HI_C_STRONG_BEAR, "Bar Color: Strong Bear", 240, 60, 60);
		return;
	}
	sc.p_GDIFunction = DrawHUD;   // set after SetDefaults so a reloaded DLL re-registers the pointer
	if (sc.LastCallToFunction) { Release(sc, -1); return; }
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	ChartState& S = Acquire(sc);
	CheckDataStamp(sc, S);
	CheckWarnings(sc, S);
	EnsureLog(sc, S);          // pulls the full chain
	BuildHudSnapshot(sc, S);

	const bool paint = sc.Input[HI_PAINT].GetYesNo() != 0;
	sc.Subgraph[0].DrawStyle = paint ? DRAWSTYLE_COLOR_BAR : DRAWSTYLE_IGNORE;
	const uint32_t cols[5] = { sc.Input[HI_C_STRONG_BEAR].GetColor(), sc.Input[HI_C_WEAK_BEAR].GetColor(), sc.Input[HI_C_NEUTRAL_BAR].GetColor(), sc.Input[HI_C_WEAK_BULL].GetColor(), sc.Input[HI_C_STRONG_BULL].GetColor() };
	int& gen = sc.GetPersistentInt(1);
	int start = sc.UpdateStartIndex;
	if (gen != S.dcs.generation) { start = 0; gen = S.dcs.generation; }
	for (int i = start; i < sc.ArraySize; ++i)
	{
		const int st = Clamp(static_cast<int>(S.dcs.barState[i]) + 2, 0, 4);
		sc.Subgraph[0][i] = paint ? 1.0f : 0.0f;
		uint32_t c = cols[st];
		if (i == sc.ArraySize - 1) c = sc.RGBInterpolate(c, RGB(0, 0, 0), 0.45f);   // forming bar: provisional, drawn dim
		sc.Subgraph[0].DataColor[i] = c;
	}
}
