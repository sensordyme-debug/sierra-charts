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
//    9. NQ Edge Terminal (the one study)      scsf_NQEdge_Terminal   <- the main chart needs only this
//   10. NQ Edge Flow Candles                 scsf_NQEdge_FlowCandles  (companion charts: order-flow candlesticks)
//   11. NQ Edge Flow CVD                     scsf_NQEdge_FlowCVD      (own panel)
//   12. NQ Edge Flow Delta                   scsf_NQEdge_FlowDelta    (own panel)
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
		int dayStartSec = 18 * 3600;   // trading-day boundary (evening open); 0 = Sierra's trading day date
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
		int compositeDays = 5;
		int adrDays = 10;
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
		float exhaustMinVolZ = 0.5f;      // exhaustion needs an above-average volume bar
		int exhaustCooldownBars = 6;      // and no same-direction exhaustion within this many bars
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
		float signalThr = 0.0f;     // 0 = from the weights file
		float fadeThr = 0.0f;
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
		int alertMinGrade = 2;      // 1 = A, 2 = A+B, 3 = all
		float minStopAtr = 0.6f;    // structural stop widened to at least this
		float maxStopAtr = 2.5f;    // candidates with a wider stop are rejected
		int minStopTicks = 6;
		int ethSignals = 1;         // outside RTH: 0 none, 1 grade A only, 2 all
		float riskPerTrade = 100.0f; // $ risk behind the size suggestion
		int tradeStartSec = 10 * 3600, tradeEndSec = 15 * 3600 + 30 * 60;   // signals only inside this window (end <= start = always)
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
		LVL_VWAP_B3U, LVL_VWAP_B3D, LVL_ABSORB, LVL_IMB, LVL_SINGLE_PRINT, LVL_PDC, LVL_OPEN, LVL_FAILED, LVL_PWH, LVL_PWL
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
		std::vector<float> pwh, pwl;                      // prior week high / low
		std::vector<float> onHigh, onLow;                 // overnight range (dev. during ON, frozen in RTH)
		std::vector<float> ibHigh, ibLow;                 // initial balance (dev. during IB, frozen after)
		std::vector<unsigned char> ibDone;
		std::vector<float> structTrend, bos, vaPos, pocPos, ibPos, valueMig, openTypeDir;
		std::vector<signed char> openType;
		std::vector<signed char> swingHighMark, swingLowMark; // 1 at the pivot bar (set at confirmation)
		std::vector<float> auctionF;                          // auction state feature
		int aucState = 0, aucDir = 0, aucSinceIdx = -1, aucBeyond = 0, aucBarsSince = 0; int aucRejectIdx = -1000, aucRejectDir = 0;
		std::vector<signed char> bosMark, chochMark;          // +1/-1 at the bar that broke
		// swings
		std::vector<Swing> swings;
		int lastSwingHigh = -1, lastSwingLow = -1;           // indices into swings
		int prevSwingHigh = -1, prevSwingLow = -1;
		bool lastHighBroken = false, lastLowBroken = false;
		// RTH profile (committed through computedThrough)
		struct PLevel { double vol = 0, bid = 0, ask = 0; };
		std::map<int, PLevel> profVol;                       // level -> volume split by aggressor
		double profTotal = 0;
		std::map<int, PLevel> prevProf; double prevProfTotal = 0;   // prior RTH session profile (ghost)
		std::vector<std::map<int, PLevel> > sessionHist;             // last N session profiles (composite)
		std::vector<float> rthRanges;                                 // RTH range per completed session (ADR)
		float prevDayClose = 0;
		std::vector<float> pdc, sessOpen;                             // per-bar prior-day close, session open
		std::map<int, int> tpoMap;                           // level -> TPO count (completed periods)
		std::set<int> periodLevels;                          // levels touched in the current TPO period
		int lastTpoPeriod = -1;
		float devPoc = 0, devVah = 0, devVal = 0;
		// day / session bookkeeping
		int curDay = 0;
		int finalizedSession = -1, rthOpenSession = -1;
		float onH = -FLT_MAX, onL = FLT_MAX;
		float rthHigh = -FLT_MAX, rthLow = FLT_MAX;
		float ibH = -FLT_MAX, ibL = FLT_MAX; bool ibClosed = false; int ibCloseIdx = -1; int ibBreakDir = 0;
		float prevDayHigh = 0, prevDayLow = 0, prevPoc = 0, prevVah = 0, prevVal = 0;
		float pwH = 0, pwL = 0, cwH = -FLT_MAX, cwL = FLT_MAX; int weekKey = -1;   // prior / current week
		float rthOpen = 0; int rthOpenIdx = -1;
		float o30High = -FLT_MAX, o30Low = FLT_MAX; int o30HighIdx = -1, o30LowIdx = -1;
		signed char todayOpenType = OT_NONE; bool openTypeDone = false; float openTypeDirValue = 0;
		std::vector<float> ibRangeHistory;                   // one per completed IB (for regime)
		std::vector<float> nakedPocs;                        // untested prior-session POCs
		std::vector<int> nakedPocBorn;
		std::vector<int> nakedPocLine;
		struct NakedRec { float price; int born; int dead; };
		std::vector<NakedRec> nakedHist;                     // every prior-session POC with its tested index (for as-of queries)
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

	struct Bubble { int idx; float price; double size; int dir; bool live; int lineNumber; int count; };

	struct FlowState : EngineCommon
	{
		std::vector<float> delta, deltaPct, cvd, cvdZ, volZ;
		std::vector<float> fAbsorb, fExhaust, fImb, fTrapped, fCvdDiv, fLarge;   // features
		std::vector<float> fLegEff, fLegVol, fPullback, fAbsorbQ;                // v2 features
		int swingPtr = 0; float lastAbsQ = 0;
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
		// swing legs (between confirmed swings) + the active leg and its regression channel
		struct Leg { int fromIdx, toIdx; float fromPrice, toPrice; double delta, volume; int ticks; bool up; bool divergence; };
		std::vector<Leg> legs; Leg active; bool activeValid = false; size_t legsSwingCount = 0;
		struct LegReg { int startIdx = -1; double slope = 0, intercept = 0, sigma = 0, r2 = 0; } legReg;
		std::vector<double> deltaPre;               // prefix sum of delta over closed bars
		std::vector<Zone> failedZones;              // supply/demand left by failed auctions (trapped traders)
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
		SCGraphData data; SCDateTimeArray times;                 // wrappers refreshed every call (no data copy)
		std::vector<double> cumPV, cumV; int cumThrough = -1;   // for ref VWAP
		std::vector<float> ema, vwap; std::vector<int> sessStart; int emaThrough = -1;
		int lastRefSize = 0;
	};

	struct InterState : EngineCommon
	{
		std::vector<float> rsYM, rsES, rsRTY, rsIndex, smt, tickCum, tickExt, tickDiv, megaCap, composite;
		std::vector<signed char> smtMark;
		RefChart ym, es, rty, tick, mega[6];
		int available = 0;                           // bitmask: 1 YM, 2 ES, 4 RTY, 8 TICK, 16.. mega
		double tickCumCommitted = 0; int tickCount = 0; int tickSession = -1;
		int lastSwingProcessed = -1;
		std::vector<double> rsPre1[3], rsPre2[3]; std::vector<float> rsRaw[3];
		std::vector<float> tickVal, tickEmaArr;
		int lastSmtIdx = -1; float lastSmtVal = 0, lastSmtPrice = 0;
		signed char megaState[6] = { 0, 0, 0, 0, 0, 0 };
		std::vector<float> leadLag;                 // feature
		std::vector<float> retP; std::vector<float> retR[3];   // 1-bar returns: primary, YM, mega1, mega2
		int leadBars = 0; float leadCorr = 0; int leadRef = -1; char leadText[32] = "";
	};

	// Feature vector published to the composite. Index constants must match kFeatureNames.
	enum FeatureId
	{
		F_VWAP_POS = 0, F_VWAP_SLOPE, F_VWAP_ACCEPT, F_STRUCT_TREND, F_BOS, F_VA_POS, F_POC_POS, F_IB_POS,
		F_VALUE_MIG, F_OPEN_TYPE, F_DELTA, F_CVD_Z, F_CVD_DIV, F_ABSORB, F_EXHAUST, F_IMBALANCE, F_TRAPPED,
		F_LARGE_TRADE, F_REGIME_TREND, F_MTF_BIAS, F_RS_INDEX, F_SMT, F_TICK_CUM, F_TICK_EXT, F_TICK_DIV,
		F_MEGA_CAP, F_LEG_EFF, F_LEG_VOL, F_PULLBACK, F_ABSORB_Q, F_AUCTION, F_LEADLAG, F_COUNT
	};
	static const char* kFeatureNames[F_COUNT] =
	{
		"vwapPos", "vwapSlope", "vwapAccept", "structTrend", "bos", "vaPos", "pocPos", "ibPos",
		"valueMig", "openType", "delta", "cvdZ", "cvdDiv", "absorb", "exhaust", "imbalance", "trapped",
		"largeTrade", "regimeTrend", "mtfBias", "rsIndex", "smt", "tickCum", "tickExt", "tickDiv", "megaCap",
		"legEff", "legVol", "pullback", "absorbQ", "auction", "leadLag"
	};
	enum FeatureGroup { G_TREND = 0, G_FLOW, G_REVERSAL, G_LOCATION, G_INTERMARKET, G_CONTEXT, G_COUNT };
	static const char* kGroupNames[G_COUNT] = { "trend", "flow", "reversal", "location", "intermarket", "context" };
	static const int kFeatureGroup[F_COUNT] =
	{
		G_LOCATION, G_TREND, G_TREND, G_TREND, G_TREND, G_LOCATION, G_LOCATION, G_LOCATION,
		G_CONTEXT, G_CONTEXT, G_FLOW, G_FLOW, G_REVERSAL, G_REVERSAL, G_REVERSAL, G_FLOW, G_REVERSAL,
		G_FLOW, G_TREND, G_TREND, G_INTERMARKET, G_REVERSAL, G_INTERMARKET, G_INTERMARKET, G_REVERSAL,
		G_INTERMARKET, G_FLOW, G_FLOW, G_LOCATION, G_REVERSAL, G_TREND, G_INTERMARKET
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
				0.5f, 1.0f, 0.9f, 0.5f, 0.7f, 0.4f, 0.4f, 0.5f, 0.5f,
				0.5f, 0.3f, 0.4f, 0.6f, 0.8f, 0.6f
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
		float dcs = 0; float rr = 0; char label[96] = ""; int grade = 2; int size = 0;
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
		std::vector<unsigned char> gPos, gAvail;   // per-bar group bitmasks: group g leans positive / has data
		std::vector<signed char> signalType, signalDir;
		Weights weights;
		std::vector<Signal> signals;
		int lastAlertIdx = -1;
		int lastSignalCheckedIdx = -1;
		bool newSignals = false;
	};

	struct SetupStats { int count = 0, wins = 0, losses = 0, t2 = 0, timeouts = 0; double sumR = 0, sumWinR = 0, sumLossR = 0, sumMfe = 0, sumMae = 0, sumBars = 0; };

	struct ValState : EngineCommon
	{
		SetupStats stats[SETUP_COUNT];
		SetupStats stats3[SETUP_COUNT][5][3];      // setup x regime x grade
		std::vector<float> cumR, addR;
		int nextSignal = 0;      // first signal index not yet fully resolved
		double totalR = 0;
	};

	struct LogRow
	{
		int idx; double t; float o, h, l, c, v; float feat[F_COUNT]; float dcs; int regime; int setup; int dir; int grade; int tradingDay;
		float atr; float fwd5, fwd15, fwd30, fwd60, mfe, mae; bool done;
	};

	struct LogState : EngineCommon
	{
		std::vector<LogRow> pending;
		int lastQueuedIdx = -1;
		bool headerWritten = false;
		int generationWritten = -1;
		int rowsWritten = 0;
		double lastWrittenTime = 0;    // survives engine resets: append mode skips rows at or before it
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
		char stateLine[160] = ""; char stateLine2[160] = "";
		int agree = 0, agreeN = 0; bool adrPrev = false; int liveSize = 0;
		char action[112] = ""; int actionKind = 0;   // 0 wait, 1 buy now, -1 sell now, 2 in long, -2 in short, 3 no trade
		int es = 0, rty = 0; bool esAvail = false, rtyAvail = false; float tickVal = 0; float liveR = 0;
		int curSetup = 0; SetupStats curStats; bool statsAvail = false;
		char vwapText[64] = "";
		int signalsTotal = 0;
		double updateMs = 0, maxUpdateMs = 0; int fullCalcMs = 0; int bars = 0;
		char dayType[24] = ""; char leadLag[32] = ""; int lastEventIdx = -1; float adrPct = 0; int interConnected = 0, interConfigured = 0; float legR2 = 0; int legDir = 0;
	};

	// ==== 14 Terminal renderer (GDI) ============================================
	// One immediate-mode renderer draws every visual layer for the visible bars only. The Backdrop
	// study calls DrawBackdrop (under the candles), the Overlay study DrawOverlay (above), the Tape
	// study DrawTape (its own strip). Geometry is computed once per paint in a Frame.

	enum Preset { PRESET_COMMAND = 0, PRESET_FOOTPRINT = 1, PRESET_CLEAN = 2 };
	enum Layer
	{
		L_CANDLES = 0, L_RIBBON, L_LEVELS, L_PROFILE, L_GHOST, L_COMPOSITE, L_ZONES, L_BUBBLES, L_SWINGS, L_CHANNEL,
		L_CARDS, L_PROJECTION, L_NOTES, L_HUD, L_FOOTPRINT, L_DEPTH, L_TINT, L_SESSION, L_SEPARATORS, L_CLOUD, L_TAPE, L_COUNT
	};
	static const char* kLayerNames[L_COUNT] =
	{
		"Conviction Candles", "DCS Ribbon", "Levels + Right-Edge Pills", "Docked Volume Profile", "Ghost Prior Profile", "Composite Profile",
		"Zones", "Large-Trade Bubbles", "Swing Delta Labels", "Regression Channel", "Signal Cards + R/R Boxes", "Projection Arrow",
		"Event Annotations", "HUD Panel", "Footprint Cells", "Depth Heatmap", "Regime Tint", "Session Shade", "Session Separators", "VWAP Cloud", "Order-Flow Tape"
	};
	// preset defaults: 1 = on, 0 = off
	static const unsigned char kPresetLayers[3][L_COUNT] =
	{
		{ 1, 1, 1, 1, 1, 0, 1, 1, 1, 1, 1, 1, 1, 1, 0, 0, 1, 1, 1, 1, 1 },   // COMMAND
		{ 1, 1, 1, 1, 1, 0, 1, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1 },   // FOOTPRINT
		{ 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 1, 1, 0, 1, 0, 0, 0, 0, 1, 0, 0 },   // CLEAN
	};

	struct Theme
	{
		uint32_t bg = RGB(11, 14, 20), grid = RGB(22, 27, 38), bull = RGB(0, 200, 150), bear = RGB(255, 77, 94), neutral = RGB(138, 147, 166);
		uint32_t gold = RGB(255, 200, 87), cyan = RGB(62, 198, 255), magenta = RGB(214, 93, 255), text = RGB(230, 234, 242), dim = RGB(92, 101, 119);
		uint32_t panel = RGB(14, 18, 26), tintUp = RGB(0, 200, 150), tintDown = RGB(255, 77, 94), tintBalance = RGB(62, 198, 255), tintChop = RGB(255, 200, 87);
	};

	// Published by the Overlay study; read by every study (regions, visibility) and renderer.
	struct VisualConfig
	{
		int preset = PRESET_COMMAND;
		bool layer[L_COUNT];
		bool diagnostics = false;
		Theme theme;
		int fontPt = 10;
		int profileWidthPct = 45;        // of the fill space
		bool profileDockRight = false;   // bars grow leftward from the right edge of the fill space
		int hudOpacity = 82;
		int maxNotes = 8;
		int minGrade = 2;                // 1 = A only, 2 = A+B, 3 = all
		int tapeRows = 6;
		VisualConfig() { preset = PRESET_CLEAN; for (int k = 0; k < L_COUNT; ++k) layer[k] = kPresetLayers[PRESET_CLEAN][k] != 0; }
	};

	struct PerfStats { double backdropMs = 0, overlayMs = 0, tapeMs = 0; int paints = 0; };

	// Drawing objects managed by line number by the Terminal study (one contiguous array so a single
	// flush can delete whatever was not redrawn in the current update).
	struct DrawSlot { int line = 0; bool used = false; };
	struct TermState
	{
		static const int HUD_SLOTS = 16, LVL_SLOTS = 6, SIG_SLOTS = 3, SWING_SLOTS = 24, ZONE_SLOTS = 8, BUBBLE_SLOTS = 30, NOTE_SLOTS = 12, PROJ_SLOTS = 3, FIB_SLOTS = 4, CHAN_SLOTS = 3, NUM_SLOTS = 60, KT_SLOTS = 12, TLN_SLOTS = 2, ABSB_SLOTS = 12;
		struct SigDraw { DrawSlot risk, reward, t1, label; };
		static const int SLOTS = HUD_SLOTS + 2 * LVL_SLOTS + 4 * SIG_SLOTS + SWING_SLOTS + ZONE_SLOTS + BUBBLE_SLOTS + NOTE_SLOTS + PROJ_SLOTS + 2 * FIB_SLOTS + CHAN_SLOTS + NUM_SLOTS + KT_SLOTS + TLN_SLOTS + ABSB_SLOTS;
		union
		{
			DrawSlot all[SLOTS];
			struct { DrawSlot hud[HUD_SLOTS]; DrawSlot lvlLine[LVL_SLOTS]; DrawSlot lvlTag[LVL_SLOTS]; SigDraw sig[SIG_SLOTS]; DrawSlot swing[SWING_SLOTS]; DrawSlot zone[ZONE_SLOTS]; DrawSlot bubble[BUBBLE_SLOTS]; DrawSlot note[NOTE_SLOTS]; DrawSlot proj[PROJ_SLOTS]; DrawSlot fibLine[FIB_SLOTS]; DrawSlot fibTag[FIB_SLOTS]; DrawSlot chan[CHAN_SLOTS]; DrawSlot num[NUM_SLOTS]; DrawSlot kt[KT_SLOTS]; DrawSlot tl[TLN_SLOTS]; DrawSlot absb[ABSB_SLOTS]; };
		};
		int regionH = 0, regionW = 0;      // price-region pixel size, captured by the GDI pass
		TermState() { for (int k = 0; k < SLOTS; ++k) { all[k].line = 0; all[k].used = false; } }
	};


	struct Event { int idx; int kind; int dir; float price; char time[8]; char text[72]; };
	enum EventKind { EV_ABSORB = 1, EV_IMB, EV_TRAP, EV_EXHAUST, EV_DIV, EV_BOS, EV_CHOCH, EV_IBBREAK, EV_SMT, EV_SIGNAL, EV_ACCEPT, EV_REJECT };

	struct DataStamp { int arraySize = 0; double t0 = 0, tMid = 0, tLast = 0; int midIdx = -1, lastIdx = -1; bool valid = false; int vIdx = -1; double vLast = 0, cMid = 0; };

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
		int hudUpdates = 0;
		VisualConfig vis;
		PerfStats perf;
		std::vector<Event> events;
		bool terminalPresent = false;      // the Terminal study owns parameters and visuals
		TermState term;

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

	inline void PushEvent(SCStudyInterfaceRef sc, ChartState& S, int idx, int kind, int dir, float price, const char* text)
	{
		Event e; e.idx = idx; e.kind = kind; e.dir = dir; e.price = price;
		int hh = 0, mm = 0, ss = 0; sc.BaseDateTimeIn[idx].GetTimeHMS(hh, mm, ss); sprintf_s(e.time, sizeof(e.time), "%02d:%02d", hh, mm);
		strncpy_s(e.text, sizeof(e.text), text, _TRUNCATE);
		S.events.push_back(e);
		if (S.events.size() > 400) S.events.erase(S.events.begin(), S.events.begin() + 100);
	}

	static std::recursive_mutex g_mutex;
	static std::map<int, ChartState*> g_charts;
	static int g_instanceId = 0;   // random per DLL load, lets study instances detect a reload
	struct RegistryCleanup { ~RegistryCleanup() { for (std::map<int, ChartState*>::iterator it = g_charts.begin(); it != g_charts.end(); ++it) delete it->second; g_charts.clear(); } };
	static RegistryCleanup g_registryCleanup;   // destroyed before g_charts/g_mutex (reverse construction order)

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
			S.auction.prevProf.clear(); S.auction.prevProfTotal = 0; S.auction.sessionHist.clear(); S.auction.rthRanges.clear(); S.auction.prevDayClose = 0;
			S.auction.devPoc = S.auction.devVah = S.auction.devVal = 0;
			S.auction.curDay = 0; S.auction.finalizedSession = -1; S.auction.rthOpenSession = -1;
			S.auction.onH = -FLT_MAX; S.auction.onL = FLT_MAX; S.auction.rthHigh = -FLT_MAX; S.auction.rthLow = FLT_MAX;
			S.auction.ibH = -FLT_MAX; S.auction.ibL = FLT_MAX; S.auction.ibClosed = false; S.auction.ibCloseIdx = -1; S.auction.ibBreakDir = 0;
			S.auction.prevDayHigh = S.auction.prevDayLow = S.auction.prevPoc = S.auction.prevVah = S.auction.prevVal = 0;
			S.auction.pwH = S.auction.pwL = 0; S.auction.cwH = -FLT_MAX; S.auction.cwL = FLT_MAX; S.auction.weekKey = -1;
			S.auction.rthOpen = 0; S.auction.rthOpenIdx = -1; S.auction.o30High = -FLT_MAX; S.auction.o30Low = FLT_MAX; S.auction.o30HighIdx = S.auction.o30LowIdx = -1;
			S.auction.todayOpenType = OT_NONE; S.auction.openTypeDone = false; S.auction.openTypeDirValue = 0;
			S.auction.aucState = 0; S.auction.aucDir = 0; S.auction.aucSinceIdx = -1; S.auction.aucBeyond = 0; S.auction.aucBarsSince = 0; S.auction.aucRejectIdx = -1000; S.auction.aucRejectDir = 0;
			S.auction.ibRangeHistory.clear();
			for (size_t k = 0; k < S.auction.nakedPocLine.size(); ++k) if (S.auction.nakedPocLine[k]) S.auction.deadLines.push_back(S.auction.nakedPocLine[k]);
			for (size_t k = 0; k < S.auction.singlePrints.size(); ++k) if (S.auction.singlePrints[k].lineNumber) S.auction.deadLines.push_back(S.auction.singlePrints[k].lineNumber);
			for (size_t k = 0; k < S.auction.liquidity.size(); ++k) if (S.auction.liquidity[k].lineNumber) S.auction.deadLines.push_back(S.auction.liquidity[k].lineNumber);
			S.auction.nakedPocs.clear(); S.auction.nakedPocBorn.clear(); S.auction.nakedPocLine.clear(); S.auction.nakedHist.clear();
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
			S.flow.legs.clear(); S.flow.activeValid = false; S.flow.legsSwingCount = 0; S.flow.legReg = FlowState::LegReg(); S.flow.failedZones.clear(); S.flow.swingPtr = 0; S.flow.lastAbsQ = 0;
			S.flow.tradeSizes.clear(); S.flow.lastTsSequence = 0; S.flow.liveSampleCount = 0; S.flow.liveThreshold = 0;
			S.flow.levelAvgSizes.clear(); S.flow.ltSampleCount = 0; S.flow.ltThreshold = 0; S.flow.lastProcessedSwing = -1; S.flow.pendingBreakouts.clear();
			S.flow.lastAbsIdx = S.flow.lastExhIdx = S.flow.lastImbIdx = S.flow.lastTrapIdx = S.flow.lastDivIdx = -1000;
			S.flow.lastAbsDir = S.flow.lastExhDir = S.flow.lastImbDir = S.flow.lastTrapDir = S.flow.lastDivDir = 0; S.flow.lastDivPrice = 0;
			S.flow.lastEventIdx = -1; S.flow.lastEventText[0] = 0;
		}
		if (engine <= E_REGIME) { S.regime.candidate = RG_NONE; S.regime.candidateCount = 0; S.regime.current = RG_NONE; for (int t = 0; t < 4; ++t) S.regime.tf[t] = RegimeState::Tf(); }
		if (engine <= E_INTER)
		{
			S.inter.tickCumCommitted = 0; S.inter.tickCount = 0; S.inter.tickSession = -1; S.inter.lastSwingProcessed = -1;
			S.inter.lastSmtIdx = -1; S.inter.lastSmtVal = 0; S.inter.lastSmtPrice = 0;
			RefChart* refs[10] = { &S.inter.ym, &S.inter.es, &S.inter.rty, &S.inter.tick, &S.inter.mega[0], &S.inter.mega[1], &S.inter.mega[2], &S.inter.mega[3], &S.inter.mega[4], &S.inter.mega[5] };
			for (int k = 0; k < 10; ++k) { refs[k]->cumThrough = -1; refs[k]->emaThrough = -1; refs[k]->cumPV.clear(); refs[k]->cumV.clear(); refs[k]->ema.clear(); refs[k]->vwap.clear(); refs[k]->sessStart.clear(); refs[k]->lastRefSize = 0; refs[k]->symbol.Clear(); }
		}
		if (engine <= E_AUCTION) S.events.clear();
		if (engine <= E_DCS) { S.dcs.signals.clear(); S.dcs.lastAlertIdx = -1; S.dcs.lastSignalCheckedIdx = -1; }
		if (engine <= E_VAL) { for (int k = 0; k < SETUP_COUNT; ++k) { S.val.stats[k] = SetupStats(); for (int r = 0; r < 5; ++r) for (int g = 0; g < 3; ++g) S.val.stats3[k][r][g] = SetupStats(); } S.val.nextSignal = 0; S.val.totalR = 0; S.val.addR.clear(); S.val.cumR.clear(); }
		if (engine <= E_LOG) { S.log.pending.clear(); S.log.lastQueuedIdx = -1; S.log.headerWritten = false; }
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
			else if (st.vIdx >= 0 && st.vIdx < n && sc.Volume[st.vIdx] != st.vLast) ok = false;      // same bar time, different content = back-fill
			else if (st.midIdx >= 0 && sc.Close[st.midIdx] != st.cMid) ok = false;
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
			st.vIdx = Max(0, st.lastIdx - 1);   // a closed bar: its volume only changes when the data is replaced
			st.vLast = sc.Volume[st.vIdx]; st.cMid = sc.Close[st.midIdx];
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
			// trading day = calendar date, rolled to the next date once the evening session has started (independent of the chart's
			// session settings; Sierra's GetTradingDayDate switched mid-evening on the user's chart)
			B.tradingDay[i] = P.dayStartSec > 0 ? (dt.GetDate() + (dt.GetTimeInSeconds() >= P.dayStartSec ? 1 : 0)) : sc.GetTradingDayDate(dt);
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

	// Trend line through two confirmed swings: the newest swing high (or low) and the most recent earlier one whose connecting
	// line no bar high (low) in between cuts by more than 0.1 ATR. `broken` = a close beyond the line after the second pivot.
	struct TrendLine
	{
		int i1 = -1, i2 = -1; float p1 = 0, p2 = 0; bool broken = false;
		float Slope() const { return i2 > i1 ? (p2 - p1) / static_cast<float>(i2 - i1) : 0.0f; }
		float At(int i) const { return p2 + Slope() * static_cast<float>(i - i2); }
	};
	inline bool FindTrendLine(SCStudyInterfaceRef sc, const ChartState& S, int lastClosed, bool highs, int maxSwings, TrendLine& out)
	{
		const AuctionState& A = S.auction; const float tol = 0.1f * AtrAt(S, lastClosed);
		std::vector<int> idx;
		for (int k = static_cast<int>(A.swings.size()) - 1; k >= 0 && static_cast<int>(idx.size()) < Max(2, maxSwings); --k)
		{
			const Swing& w = A.swings[k];
			if (w.high != highs || w.confirmIdx > lastClosed || w.idx < 0 || w.idx > lastClosed) continue;
			idx.push_back(k);
		}
		if (idx.size() < 2) return false;
		const Swing& s2 = A.swings[idx[0]];
		for (size_t q = 1; q < idx.size(); ++q)
		{
			const Swing& s1 = A.swings[idx[q]];
			if (s2.idx - s1.idx < 3) continue;
			const float slope = (s2.price - s1.price) / static_cast<float>(s2.idx - s1.idx);
			bool ok = true;
			for (int i = s1.idx + 1; i < s2.idx && ok; ++i) { const float lv = s1.price + slope * static_cast<float>(i - s1.idx); if (highs ? sc.High[i] > lv + tol : sc.Low[i] < lv - tol) ok = false; }
			if (!ok) continue;
			out.i1 = s1.idx; out.i2 = s2.idx; out.p1 = s1.price; out.p2 = s2.price; out.broken = false;
			for (int i = s2.idx + 1; i <= lastClosed; ++i) { const float lv = out.At(i); if (highs ? sc.Close[i] > lv + tol : sc.Close[i] < lv - tol) { out.broken = true; break; } }
			return true;
		}
		return false;
	}

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
	void EnsureInter(SCStudyInterfaceRef sc, ChartState& S);
	void EnsureDcs(SCStudyInterfaceRef sc, ChartState& S);
	void EnsureVal(SCStudyInterfaceRef sc, ChartState& S);
	void EnsureLog(SCStudyInterfaceRef sc, ChartState& S);

	// ==== 6  Auction / Structure engine ========================================
	namespace auction_detail
	{
		typedef std::map<int, AuctionState::PLevel> LevelMap;

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
						AuctionState::PLevel& L = vol[level]; L.vol += p->Volume; L.bid += p->BidVolume; L.ask += p->AskVolume; total += p->Volume;
					}
				}
			}
			if (!used)
			{
				const double v = sc.Volume[i];
				const int levels = Max(1, r_hi - r_lo + 1);
				const double bid = sc.BidVolume[i], ask = sc.AskVolume[i];
				for (int lv = r_lo; lv <= r_hi; ++lv) { AuctionState::PLevel& L = vol[lv]; L.vol += v / levels; L.bid += bid / levels; L.ask += ask / levels; }
				total += v;
			}
		}

		// POC and value area (two-levels-at-a-time expansion from the POC).
		bool ComputePocVa(const LevelMap& vol, double total, float vaPct, int& poc, int& vah, int& val)
		{
			if (vol.empty() || total <= 0) return false;
			std::vector<std::pair<int, double> > v; v.reserve(vol.size());
			for (LevelMap::const_iterator it = vol.begin(); it != vol.end(); ++it) v.push_back(std::make_pair(it->first, it->second.vol));
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

		void FinalizeRthSession(ChartState& S, int atIdx, float lastClose)
		{
			AuctionState& A = S.auction;
			A.prevDayClose = lastClose;
			if (A.profTotal > 0)
			{
				A.prevProf = A.profVol; A.prevProfTotal = A.profTotal;
				A.sessionHist.push_back(A.profVol); if (A.sessionHist.size() > 30) A.sessionHist.erase(A.sessionHist.begin());
				int poc, vah, val;
				if (ComputePocVa(A.profVol, A.profTotal, S.params.auction.valueAreaPct, poc, vah, val))
				{
					const int tpl = Max(1, S.params.auction.profileTicksPerLevel);
					A.prevPoc = LevelMid(poc, S.tickSize, tpl); A.prevVah = LevelTop(vah, S.tickSize, tpl); A.prevVal = LevelBottom(val, S.tickSize, tpl);
					A.nakedPocs.push_back(A.prevPoc); A.nakedPocBorn.push_back(atIdx); A.nakedPocLine.push_back(0);
					{ AuctionState::NakedRec rec; rec.price = A.prevPoc; rec.born = atIdx; rec.dead = -1; A.nakedHist.push_back(rec); if (A.nakedHist.size() > 400) A.nakedHist.erase(A.nakedHist.begin(), A.nakedHist.begin() + 100); }
					const int keep = Max(0, S.params.auction.nakedPocsTracked);
					while (static_cast<int>(A.nakedPocs.size()) > keep)
					{
						if (A.nakedPocLine[0] != 0) A.deadLines.push_back(A.nakedPocLine[0]);
						A.nakedPocs.erase(A.nakedPocs.begin()); A.nakedPocBorn.erase(A.nakedPocBorn.begin()); A.nakedPocLine.erase(A.nakedPocLine.begin());
					}
				}
			}
			if (A.rthHigh > -FLT_MAX) { A.prevDayHigh = A.rthHigh; A.prevDayLow = A.rthLow; A.rthRanges.push_back(A.rthHigh - A.rthLow); if (A.rthRanges.size() > 60) A.rthRanges.erase(A.rthRanges.begin()); }
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
		Fit(A.poc, n); Fit(A.vah, n); Fit(A.val, n); Fit(A.pdPoc, n); Fit(A.pdVah, n); Fit(A.pdVal, n); Fit(A.pdh, n); Fit(A.pdl, n); Fit(A.pwh, n); Fit(A.pwl, n);
		Fit(A.onHigh, n); Fit(A.onLow, n); Fit(A.ibHigh, n); Fit(A.ibLow, n); Fit(A.ibDone, n);
		Fit(A.structTrend, n); Fit(A.bos, n); Fit(A.vaPos, n); Fit(A.pocPos, n); Fit(A.ibPos, n); Fit(A.valueMig, n); Fit(A.openTypeDir, n);
		Fit(A.openType, n); Fit(A.swingHighMark, n); Fit(A.swingLowMark, n); Fit(A.bosMark, n); Fit(A.chochMark, n); Fit(A.pdc, n); Fit(A.sessOpen, n); Fit(A.auctionF, n);
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
			if (prevRth && (!rth || newDay) && A.finalizedSession != B.rthSession[i - 1]) { A.finalizedSession = B.rthSession[i - 1]; FinalizeRthSession(S, i, sc.Close[i - 1]); }
			if (newDay && A.curDay != B.tradingDay[i])
			{
				A.curDay = B.tradingDay[i];
				A.onH = -FLT_MAX; A.onL = FLT_MAX;
			}
			{
				const int wk = (B.tradingDay[i] - 1) / 7;   // Sunday-based week of the trading-day date
				if (wk != A.weekKey) { if (A.cwH > -FLT_MAX) { A.pwH = A.cwH; A.pwL = A.cwL; } A.cwH = -FLT_MAX; A.cwL = FLT_MAX; A.weekKey = wk; }
			}
			if (rth && !prevRth && A.rthOpenIdx != i && A.rthOpenSession != B.rthSession[i])
			{
				A.rthOpenSession = B.rthSession[i]; A.rthOpen = sc.Open[i]; A.rthOpenIdx = i; A.todayOpenType = OT_NONE; A.openTypeDone = false;
				A.o30High = -FLT_MAX; A.o30Low = FLT_MAX; A.o30HighIdx = A.o30LowIdx = -1;
				A.profVol.clear(); A.profTotal = 0; A.tpoMap.clear(); A.periodLevels.clear(); A.lastTpoPeriod = -1;
				A.rthHigh = -FLT_MAX; A.rthLow = FLT_MAX; A.ibH = -FLT_MAX; A.ibL = FLT_MAX; A.ibClosed = false; A.ibCloseIdx = -1; A.ibBreakDir = 0;
				A.aucState = 0; A.aucDir = 0; A.aucSinceIdx = -1; A.aucBeyond = 0; A.aucBarsSince = 0;
			}

			// ---- provisional copies of the committed accumulators for this bar ----
			float onH = A.onH, onL = A.onL, ibH = A.ibH, ibL = A.ibL, rthHigh = A.rthHigh, rthLow = A.rthLow;
			bool ibClosed = A.ibClosed;
			{ const float cwH = Max(A.cwH, h), cwL = Min(A.cwL, l); if (closed) { A.cwH = cwH; A.cwL = cwL; } }
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
					if (prevTrend < 0) { A.chochMark[i] = 1; A.lastChochDir = 1; A.lastChochIdx = i; PushEvent(sc, S, i, EV_CHOCH, 1, c, "CHoCH up"); } else { A.bosMark[i] = 1; PushEvent(sc, S, i, EV_BOS, 1, c, "BOS up"); }
					A.lastBosDir = 1; A.lastBosIdx = i;
				}
				if (A.lastSwingLow >= 0 && !A.lastLowBroken && A.swings[A.lastSwingLow].idx < i && c < A.swings[A.lastSwingLow].price)
				{
					A.lastLowBroken = true;
					if (prevTrend > 0) { A.chochMark[i] = -1; A.lastChochDir = -1; A.lastChochIdx = i; PushEvent(sc, S, i, EV_CHOCH, -1, c, "CHoCH down"); } else { A.bosMark[i] = -1; PushEvent(sc, S, i, EV_BOS, -1, c, "BOS down"); }
					A.lastBosDir = -1; A.lastBosIdx = i;
				}
				// first close beyond the initial balance
				if (rth && ibClosed && ibH > ibL && A.ibBreakDir == 0 && (c > ibH || c < ibL))
				{
					A.ibBreakDir = c > ibH ? 1 : -1; char t[64]; sprintf_s(t, sizeof(t), "IB break %s @ %s", c > ibH ? "up" : "down", sc.FormatGraphValue(c, sc.BaseGraphValueFormat).GetChars());
					PushEvent(sc, S, i, EV_IBBREAK, A.ibBreakDir, c, t);
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
						for (int q = static_cast<int>(A.nakedHist.size()) - 1; q >= 0; --q) if (A.nakedHist[q].dead < 0 && A.nakedHist[q].born == A.nakedPocBorn[k]) { A.nakedHist[q].dead = i; break; }
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
			A.pdc[i] = A.prevDayClose; A.sessOpen[i] = (rth && A.rthOpenIdx >= 0) ? A.rthOpen : 0.0f;
			A.onHigh[i] = onH > -FLT_MAX ? onH : 0.0f; A.onLow[i] = onL < FLT_MAX ? onL : 0.0f;
			A.pwh[i] = A.pwH; A.pwl[i] = A.pwL;
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
			// ---- auction state machine: balance -> initiative breakout -> acceptance / rejection ----
			if (closed && rth && (vah > val || (ibClosed && ibH > ibL)))
			{
				const float upEdge = Max(vah > val ? vah : -FLT_MAX, ibClosed ? ibH : -FLT_MAX), dnEdge = Min(vah > val ? val : FLT_MAX, ibClosed ? ibL : FLT_MAX);
				const int beyond = c > upEdge ? 1 : (c < dnEdge ? -1 : 0);
				if (A.aucState == 0 && beyond != 0) { A.aucState = 1; A.aucDir = beyond; A.aucSinceIdx = i; A.aucBeyond = 1; A.aucBarsSince = 1; }
				else if (A.aucState >= 1)
				{
					++A.aucBarsSince; if (beyond == A.aucDir) ++A.aucBeyond;
					const float share = static_cast<float>(A.aucBeyond) / Max(1, A.aucBarsSince);
					if (A.aucState == 1 && A.aucBeyond >= 3 && share >= 0.6f) { A.aucState = 2; char t[64]; sprintf_s(t, sizeof(t), "acceptance %s value", A.aucDir > 0 ? "above" : "below"); PushEvent(sc, S, i, EV_ACCEPT, A.aucDir, c, t); }
					else if (A.aucState == 1 && beyond != A.aucDir && A.aucBarsSince <= 4) { A.aucState = 0; A.aucRejectIdx = i; A.aucRejectDir = -A.aucDir; char t[64]; sprintf_s(t, sizeof(t), "rejection: back inside value"); PushEvent(sc, S, i, EV_REJECT, -A.aucDir, c, t); A.aucDir = 0; }
					else if (A.aucState == 2 && beyond == -A.aucDir) { A.aucState = 0; A.aucRejectIdx = i; A.aucRejectDir = -A.aucDir; A.aucDir = 0; }
					else if (A.aucState >= 1 && beyond == 0 && share < 0.3f && A.aucBarsSince > 10) { A.aucState = 0; A.aucDir = 0; }
				}
			}
			{
				float af = 0;
				if (A.aucState == 1) af = 0.5f * A.aucDir; else if (A.aucState == 2) af = 1.0f * A.aucDir;
				if (A.aucRejectIdx >= 0 && i - A.aucRejectIdx <= 10 && A.aucState == 0) af = 0.5f * A.aucRejectDir * static_cast<float>(exp(-(i - A.aucRejectIdx) / 5.0));
				A.auctionF[i] = rth ? af : 0.0f;
			}
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
		inline void Note(SCStudyInterfaceRef sc, ChartState& S, int idx, int kind, int dir, float price, const char* text)
		{
			char buf[72]; sprintf_s(buf, sizeof(buf), "%s @ %s", text, sc.FormatGraphValue(price, sc.BaseGraphValueFormat).GetChars());
			PushEvent(sc, S, idx, kind, dir, price, buf);
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
		Fit(F.fLegEff, n); Fit(F.fLegVol, n); Fit(F.fPullback, n); Fit(F.fAbsorbQ, n);
		Fit(F.absorbMark, n); Fit(F.exhaustMark, n); Fit(F.imbMark, n); Fit(F.trapMark, n); Fit(F.divMark, n);
		Fit(F.volPre1, n); Fit(F.volPre2, n); Fit(F.dzPre1, n); Fit(F.dzPre2, n); Fit(F.cvdDz, n); Fit(F.deltaPre, n);
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
						if (bq.idx == bi && bq.live && bq.dir == dir && fabs(bq.price - price) < 0.5f * tick) { bq.size += size; ++bq.count; merged = true; break; }
					}
					if (!merged) { Bubble bb; bb.idx = bi; bb.price = price; bb.size = size; bb.dir = dir; bb.live = true; bb.lineNumber = 0; bb.count = 1; F.bubbles.push_back(bb); }
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
				F.deltaPre[i] = (i > 0 ? F.deltaPre[i - 1] : 0.0) + delta;
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
					const float closePosB = range > 0 ? (c - l) / range : 0.5f, closePosS = range > 0 ? (h - c) / range : 0.5f;
					int touches = 0; for (int z = static_cast<int>(F.absorbZones.size()) - 1; z >= 0 && F.absorbZones[z].bornIdx >= i - 50; --z) if (fabs(0.5f * (F.absorbZones[z].top + F.absorbZones[z].bottom) - (F.deltaPct[i] <= -0.10f ? l : h)) <= 0.5f * atr) ++touches;
					if (F.deltaPct[i] <= -0.10f && (c - l) >= 0.4f * range && l <= prevLow)
					{
						F.lastAbsQ = Clamp1(F.volZ[i] * (1.0f - range / atr) * closePosB * (1.0f + 0.25f * touches) / 2.5f);
						Zone z; z.bornIdx = i; z.bottom = l; z.top = l + P.absorbZoneFrac * range; z.dir = 1; z.kind = LVL_ABSORB; z.active = true;
						F.absorbZones.push_back(z); F.absorbMark[i] = 1; F.lastAbsIdx = i; F.lastAbsDir = 1; SetEvent(F, i, l, "Absorption (buyers)"); Note(sc, S, i, EV_ABSORB, 1, l, "absorb: buyers");
					}
					else if (F.deltaPct[i] >= 0.10f && (h - c) >= 0.4f * range && h >= prevHigh)
					{
						F.lastAbsQ = Clamp1(F.volZ[i] * (1.0f - range / atr) * closePosS * (1.0f + 0.25f * touches) / 2.5f);
						Zone z; z.bornIdx = i; z.top = h; z.bottom = h - P.absorbZoneFrac * range; z.dir = -1; z.kind = LVL_ABSORB; z.active = true;
						F.absorbZones.push_back(z); F.absorbMark[i] = -1; F.lastAbsIdx = i; F.lastAbsDir = -1; SetEvent(F, i, h, "Absorption (sellers)"); Note(sc, S, i, EV_ABSORB, -1, h, "absorb: sellers");
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
								if (upRun && F.volZ[i] >= P.exhaustMinVolZ && (h - c) >= 0.3f * (h - l) && (F.lastExhDir != -1 || i - F.lastExhIdx > P.exhaustCooldownBars) && lv[m - 1]->Volume <= thin && (m < 3 || lv[m - 2]->Volume <= thin * 1.5)) { F.exhaustMark[i] = -1; F.lastExhIdx = i; F.lastExhDir = -1; SetEvent(F, i, h, "Exhaustion top"); PushMarker(F, i, -1, 3, h); Note(sc, S, i, EV_EXHAUST, -1, h, "exhaust top"); }
								else if (downRun && F.volZ[i] >= P.exhaustMinVolZ && (c - l) >= 0.3f * (h - l) && (F.lastExhDir != 1 || i - F.lastExhIdx > P.exhaustCooldownBars) && lv[0]->Volume <= thin && (m < 3 || lv[1]->Volume <= thin * 1.5)) { F.exhaustMark[i] = 1; F.lastExhIdx = i; F.lastExhDir = 1; SetEvent(F, i, l, "Exhaustion bottom"); PushMarker(F, i, 1, 3, l); Note(sc, S, i, EV_EXHAUST, 1, l, "exhaust bottom"); }
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
									F.imbZones.push_back(z); F.imbMark[i] = static_cast<signed char>(F.imbMark[i] == -1 ? 2 : 1); F.lastImbIdx = i; F.lastImbDir = 1; SetEvent(F, i, z.bottom, "Stacked buy imbalance"); Note(sc, S, i, EV_IMB, 1, z.bottom, "buy imb stack");
									buyRun = 0;
								}
								if (sellRun >= P.imbStackLevels && (q + 1 >= m || !(q + 2 < m && lv[q + 2]->PriceInTicks - lv[q + 1]->PriceInTicks == 1 && lv[q + 1]->BidVolume >= minV && lv[q + 2]->AskVolume >= minV && lv[q + 1]->BidVolume >= ratio * lv[q + 2]->AskVolume)))
								{
									Zone z; z.bornIdx = i; z.bottom = (lv[sellStart]->PriceInTicks - 0.5f) * tick; z.top = (lv[q]->PriceInTicks + 0.5f) * tick; z.dir = -1; z.kind = LVL_IMB; z.active = true;
									F.imbZones.push_back(z); F.imbMark[i] = static_cast<signed char>(F.imbMark[i] == 1 ? 2 : -1); F.lastImbIdx = i; F.lastImbDir = -1; SetEvent(F, i, z.top, "Stacked sell imbalance"); Note(sc, S, i, EV_IMB, -1, z.top, "sell imb stack");
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
									Bubble bb; bb.idx = i; bb.price = lv[q]->PriceInTicks * tick; bb.size = lv[q]->Volume; bb.dir = lv[q]->AskVolume >= lv[q]->BidVolume ? 1 : -1; bb.live = false; bb.lineNumber = 0; bb.count = Max(1, static_cast<int>(lv[q]->NumberOfTrades));
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
				for (size_t z = 0; z < F.failedZones.size(); ++z) { Zone& Z = F.failedZones[z]; if (Z.active && i > Z.bornIdx && ((Z.dir > 0 && c < Z.bottom) || (Z.dir < 0 && c > Z.top))) { Z.active = false; Z.deadIdx = i; } }

				// trapped traders: strong-delta breakout fully reversed within K bars
				{
					const int LB = Max(3, P.trapLookback);
					for (size_t q = 0; q < F.pendingBreakouts.size(); )
					{
						FlowState::Breakout& bo = F.pendingBreakouts[q];
						bool done = false;
						if (i - bo.idx > P.trapReversalBars) done = true;
						else if (bo.dir > 0 && c < bo.extreme)
						{
							F.trapMark[i] = -1; F.lastTrapIdx = i; F.lastTrapDir = -1; SetEvent(F, i, bo.extreme, "Trapped longs"); PushMarker(F, i, -1, 2, h); done = true; Note(sc, S, i, EV_TRAP, -1, bo.extreme, "trapped longs");
							float hh = -FLT_MAX; for (int q = bo.idx; q <= i; ++q) hh = Max(hh, sc.High[q]);
							Zone z; z.bornIdx = i; z.top = hh; z.bottom = Max(bo.extreme, hh - 0.35f * atr); z.dir = -1; z.kind = LVL_FAILED; z.active = true; F.failedZones.push_back(z);
						}
						else if (bo.dir < 0 && c > bo.extreme)
						{
							F.trapMark[i] = 1; F.lastTrapIdx = i; F.lastTrapDir = 1; SetEvent(F, i, bo.extreme, "Trapped shorts"); PushMarker(F, i, 1, 2, l); done = true; Note(sc, S, i, EV_TRAP, 1, bo.extreme, "trapped shorts");
							float ll = FLT_MAX; for (int q = bo.idx; q <= i; ++q) ll = Min(ll, sc.Low[q]);
							Zone z; z.bornIdx = i; z.bottom = ll; z.top = Min(bo.extreme, ll + 0.35f * atr); z.dir = 1; z.kind = LVL_FAILED; z.active = true; F.failedZones.push_back(z);
						}
						while (static_cast<int>(F.failedZones.size()) > Max(5, P.maxActiveZones)) F.failedZones.erase(F.failedZones.begin());
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
							if (ns.high && ns.price > ps.price && cvdNew <= cvdOld) { F.divMark[i] = -1; F.lastDivIdx = i; F.lastDivDir = -1; F.lastDivPrice = ns.price; PushMarker(F, ns.idx, -1, 1, ns.price); Note(sc, S, ns.idx, EV_DIV, -1, ns.price, "CVD divergence: HH, weaker delta"); }
							else if (!ns.high && ns.price < ps.price && cvdNew >= cvdOld) { F.divMark[i] = 1; F.lastDivIdx = i; F.lastDivDir = 1; F.lastDivPrice = ns.price; PushMarker(F, ns.idx, 1, 1, ns.price); Note(sc, S, ns.idx, EV_DIV, 1, ns.price, "CVD divergence: LL, stronger delta"); }
						}
					}
				}
			}

			// ---- leg-based features as of bar i (swings confirmed at or before i) ----
			{
				while (F.swingPtr < static_cast<int>(A.swings.size()) && A.swings[F.swingPtr].confirmIdx <= i) ++F.swingPtr;
				const int k = F.swingPtr - 1;             // last confirmed swing as of i
				float legEff = 0, legVol = 0, pull = 0;
				if (k >= 0 && A.swings[k].idx < i && i > 0)
				{
					const Swing& sw = A.swings[k];
					const int from = sw.idx;
					const double legDelta = (closed ? F.deltaPre[i] : F.deltaPre[i - 1] + delta) - F.deltaPre[from];
					const double legVolume = (closed ? F.volPre1[i] : F.volPre1[i - 1] + vol) - F.volPre1[from];
					const float legTicks = Max(1.0f, static_cast<float>(fabs(c - sw.price) / Max(tick, 1e-6f)));
					double volMean = 0; { double m = 0, sd = 0; if (WindowStats(F.volPre1, F.volPre2, i - 1, L, m, sd)) volMean = m; }
					const float ticksPerBar = Max(1.0f, atr / Max(tick, 1e-6f));
					if (volMean > 0) legEff = Clamp1((legDelta / legTicks) / (volMean / ticksPerBar));
					if (k >= 1)
					{
						const Swing& pv = A.swings[k - 1];
						const double prevVol = F.volPre1[Min(sw.idx, i)] - F.volPre1[pv.idx];
						if (prevVol > 0 && legVolume > 0) legVol = Clamp1(log(legVolume / prevVol) / log(2.0) / 2.0) * (c >= sw.price ? 1.0f : -1.0f);
						const float prevRange = static_cast<float>(fabs(sw.price - pv.price));
						const bool prevUp = sw.price > pv.price, activeDown = c < sw.price;
						if (prevRange > 0 && prevUp == activeDown)   // active leg pulls back against the prior leg
						{
							const float depth = static_cast<float>(fabs(c - sw.price)) / prevRange;
							pull = Clamp1(1.0 - 2.0 * depth) * (prevUp ? 1.0f : -1.0f);
						}
					}
				}
				F.fLegEff[i] = legEff; F.fLegVol[i] = legVol; F.fPullback[i] = pull;
			}
			// ---- features ----
			const int decay = Max(1, P.eventDecayBars);
			F.fAbsorb[i] = Decayed(F.lastAbsDir, F.lastAbsIdx, i, decay);
			F.fAbsorbQ[i] = Decayed(F.lastAbsDir, F.lastAbsIdx, i, decay) * static_cast<float>(fabs(F.lastAbsQ));
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
		// ---- swing legs (rebuilt when the swing list changes) + active leg + regression ----
		{
			const int lc = n - 2;
			if (A.swings.size() != F.legsSwingCount && lc >= 0)
			{
				F.legsSwingCount = A.swings.size(); F.legs.clear();
				const int first = Max(1, static_cast<int>(A.swings.size()) - 80);
				for (int k = first; k < static_cast<int>(A.swings.size()); ++k)
				{
					const Swing& a = A.swings[k - 1]; const Swing& b = A.swings[k];
					if (b.confirmIdx > lc || b.idx <= a.idx) continue;
					FlowState::Leg L; L.fromIdx = a.idx; L.toIdx = b.idx; L.fromPrice = a.price; L.toPrice = b.price;
					L.delta = F.deltaPre[Min(b.idx, lc)] - F.deltaPre[a.idx]; L.volume = F.volPre1[Min(b.idx, lc)] - F.volPre1[a.idx];
					L.ticks = static_cast<int>(fabs(b.price - a.price) / Max(tick, 1e-6f) + 0.5f); L.up = b.price > a.price; L.divergence = false;
					if (F.legs.size() >= 2)
					{
						const FlowState::Leg& prev = F.legs[F.legs.size() - 2];   // same direction leg
						if (prev.up == L.up && ((L.up && L.toPrice > prev.toPrice && L.delta < prev.delta) || (!L.up && L.toPrice < prev.toPrice && L.delta > prev.delta))) L.divergence = true;
					}
					F.legs.push_back(L);
				}
			}
			F.activeValid = false; F.legReg = FlowState::LegReg();
			if (!A.swings.empty() && lc >= 0)
			{
				const Swing& a = A.swings.back();
				if (a.idx < lc)
				{
					FlowState::Leg& L = F.active;
					L.fromIdx = a.idx; L.toIdx = lc; L.fromPrice = a.price; L.toPrice = sc.Close[lc];
					L.delta = F.deltaPre[lc] - F.deltaPre[a.idx]; L.volume = F.volPre1[lc] - F.volPre1[a.idx];
					L.ticks = static_cast<int>(fabs(L.toPrice - L.fromPrice) / Max(tick, 1e-6f) + 0.5f); L.up = L.toPrice > L.fromPrice; L.divergence = false;
					F.activeValid = true;
					// linear regression of closes over the active leg (capped)
					const int s0 = Max(a.idx, lc - 500), m = lc - s0 + 1;
					if (m >= 5)
					{
						double sx = 0, sy = 0, sxx = 0, sxy = 0;
						for (int q = 0; q < m; ++q) { const double y = sc.Close[s0 + q]; sx += q; sy += y; sxx += static_cast<double>(q) * q; sxy += q * y; }
						const double den = m * sxx - sx * sx;
						if (den > 0)
						{
							const double slope = (m * sxy - sx * sy) / den, icpt = (sy - slope * sx) / m;
							double ssr = 0, sst = 0; const double ym = sy / m;
							for (int q = 0; q < m; ++q) { const double y = sc.Close[s0 + q]; const double e = y - (icpt + slope * q); ssr += e * e; sst += (y - ym) * (y - ym); }
							F.legReg.startIdx = s0; F.legReg.slope = slope; F.legReg.intercept = icpt; F.legReg.sigma = sqrt(ssr / m); F.legReg.r2 = sst > 0 ? 1.0 - ssr / sst : 0.0;
						}
					}
				}
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
			if (i < static_cast<int>(A.auctionF.size()) && fabs(A.auctionF[i]) >= 1.0f) trendiness += 0.1f;
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

	// ==== 10 Intermarket engine =================================================
	namespace inter_detail
	{
		// Loads / refreshes a reference chart: arrays, session starts, cumulative VWAP sums, EMA.
		// Returns false when the chart has no data.
		bool RefreshRef(SCStudyInterfaceRef sc, ChartState& S, RefChart& R, int chartNumber, int emaLen)
		{
			R.chartNumber = chartNumber; R.ok = false;
			if (chartNumber <= 0) return false;
			sc.GetChartBaseData(chartNumber, R.data);
			sc.GetChartDateTimeArray(chartNumber, R.times);
			const int rn = R.data[SC_LAST].GetArraySize();
			if (rn <= 2) return false;
			if (R.symbol.IsEmpty()) R.symbol = sc.GetChartSymbol(chartNumber);
			if (rn < R.lastRefSize) { R.cumThrough = -1; }   // reload
			Fit(R.cumPV, rn); Fit(R.cumV, rn); Fit(R.ema, rn); Fit(R.sessStart, rn); Fit(R.vwap, rn);
			const BaseParams& BP = S.params.base;
			const float a = 2.0f / (Max(2, emaLen) + 1.0f);
			int from = R.cumThrough + 1; if (from < 0) from = 0;
			for (int j = from; j < rn; ++j)
			{
				const SCDateTime& dt = R.times[j];
				const int tod = dt.GetTimeInSeconds();
				const bool rth = (BP.rthStartSec < BP.rthEndSec) ? (tod >= BP.rthStartSec && tod < BP.rthEndSec) : (tod >= BP.rthStartSec || tod < BP.rthEndSec);
				const int key = rth ? dt.GetDate() : -1;
				const int prevKey = (j > 0) ? ((R.sessStart[j - 1] >= 0) ? R.times[R.sessStart[j - 1]].GetDate() : -1) : -2;
				const bool newSess = (j == 0) || key < 0 || key != prevKey || R.sessStart[j - 1] < 0;
				R.sessStart[j] = (key < 0) ? -1 : (newSess ? j : R.sessStart[j - 1]);
				const double v = R.data[SC_VOLUME][j];
				const double tp = (R.data[SC_HIGH][j] + R.data[SC_LOW][j] + R.data[SC_LAST][j]) / 3.0;
				R.cumPV[j] = (j > 0 ? R.cumPV[j - 1] : 0.0) + tp * v;
				R.cumV[j] = (j > 0 ? R.cumV[j - 1] : 0.0) + v;
				if (R.sessStart[j] >= 0)
				{
					const int s0 = R.sessStart[j];
					const double pv = R.cumPV[j] - (s0 > 0 ? R.cumPV[s0 - 1] : 0.0), vv = R.cumV[j] - (s0 > 0 ? R.cumV[s0 - 1] : 0.0);
					R.vwap[j] = vv > 0 ? static_cast<float>(pv / vv) : 0.0f;
				}
				else R.vwap[j] = 0;
				R.ema[j] = (j == 0) ? R.data[SC_LAST][j] : R.ema[j - 1] + a * (R.data[SC_LAST][j] - R.ema[j - 1]);
			}
			R.cumThrough = rn - 2;      // the last reference bar is re-evaluated every call
			R.lastRefSize = rn;
			R.ok = true;
			return true;
		}

		// Reference bar that had closed by the time primary bar i closed.
		inline int Align(SCStudyInterfaceRef sc, const RefChart& R, int i, int n)
		{
			const int rn = R.data[SC_LAST].GetArraySize();
			if (rn <= 0) return -1;
			int j = sc.GetContainingIndexForSCDateTime(R.chartNumber, sc.BaseDateTimeIn[i]);
			if (j < 0) return -1; if (j >= rn) j = rn - 1;
			if (i + 1 < n && j + 1 < rn && R.times[j + 1] > sc.BaseDateTimeIn[i + 1] && j > 0) --j;   // reference bar still forming when i closed
			return j;
		}

		inline float RefHighBetween(const RefChart& R, int j0, int j1, bool high)
		{
			float v = high ? -FLT_MAX : FLT_MAX;
			const int rn = R.data[SC_LAST].GetArraySize();
			for (int j = Max(0, j0); j <= Min(rn - 1, j1); ++j) v = high ? Max(v, R.data[SC_HIGH][j]) : Min(v, R.data[SC_LOW][j]);
			return v;
		}
	}

	void EnsureInter(SCStudyInterfaceRef sc, ChartState& S)
	{
		using namespace inter_detail;
		EnsureRegime(sc, S);
		const int n = sc.ArraySize; if (n <= 0) return;
		InterState& I = S.inter; const InterParams& P = S.params.inter; const BaseState& B = S.base; const AuctionState& A = S.auction;
		Fit(I.rsYM, n); Fit(I.rsES, n); Fit(I.rsRTY, n); Fit(I.rsIndex, n); Fit(I.smt, n); Fit(I.tickCum, n); Fit(I.tickExt, n);
		Fit(I.tickDiv, n); Fit(I.megaCap, n); Fit(I.composite, n); Fit(I.smtMark, n);
		for (int k = 0; k < 3; ++k) { Fit(I.rsPre1[k], n); Fit(I.rsPre2[k], n); Fit(I.rsRaw[k], n); }
		Fit(I.tickVal, n); Fit(I.tickEmaArr, n); Fit(I.leadLag, n); Fit(I.retP, n); for (int k = 0; k < 3; ++k) Fit(I.retR[k], n);
		if (I.UpToDate(sc)) return;
		int from = I.computedThrough + 1; if (from < 0) from = 0; if (from > n - 1) from = n - 1;

		// ---- reference charts ----
		RefChart* idx[3] = { &I.ym, &I.es, &I.rty };
		const int idxCharts[3] = { P.chartYM, P.chartES, P.chartRTY };
		I.available = 0;
		char missing[160] = "";
		for (int k = 0; k < 3; ++k)
		{
			if (RefreshRef(sc, S, *idx[k], idxCharts[k], P.megaEma)) I.available |= (1 << k);
			else if (idxCharts[k] > 0) { strcat_s(missing, sizeof(missing), k == 0 ? "YM " : (k == 1 ? "ES " : "RTY ")); }
		}
		if (RefreshRef(sc, S, I.tick, P.chartTICK, P.tickEma)) I.available |= 8; else if (P.chartTICK > 0) strcat_s(missing, sizeof(missing), "TICK ");
		int megaN = 0;
		for (int k = 0; k < 6; ++k)
		{
			if (RefreshRef(sc, S, I.mega[k], P.chartMega[k], P.megaEma)) { I.available |= (16 << k); ++megaN; }
			else if (P.chartMega[k] > 0) { char b[32]; sprintf_s(b, sizeof(b), "MegaCap%d ", k + 1); strcat_s(missing, sizeof(missing), b); }
		}
		strncpy_s(S.warn.interMissing, sizeof(S.warn.interMissing), missing, _TRUNCATE);
		const int L = Max(2, P.rsLookback), Z = Max(10, P.rsZLength);

		for (int i = from; i < n; ++i)
		{
			const bool closed = (i <= n - 2);
			const float c = sc.Close[i];
			const float atr = AtrAt(S, i);

			// ---- relative strength vs each index ----
			float rsSum = 0; int rsCnt = 0;
			float rsOut[3] = { kNaN, kNaN, kNaN };
			for (int k = 0; k < 3; ++k)
			{
				RefChart& R = *idx[k];
				if (!R.ok) { I.rsRaw[k][i] = 0; if (closed) { I.rsPre1[k][i] = i > 0 ? I.rsPre1[k][i - 1] : 0; I.rsPre2[k][i] = i > 0 ? I.rsPre2[k][i - 1] : 0; } continue; }
				const int j = Align(sc, R, i, n);
				const int jPrev = (i - L >= 0) ? Align(sc, R, i - L, n) : -1;
				double rs = 0;
				if (j >= 0 && jPrev >= 0 && i - L >= 0 && sc.Close[i - L] > 0 && R.data[SC_LAST][jPrev] > 0)
					rs = (c / sc.Close[i - L] - 1.0) - (R.data[SC_LAST][j] / R.data[SC_LAST][jPrev] - 1.0);
				I.rsRaw[k][i] = static_cast<float>(rs);
				// z-score vs the last Z closed values
				double mean = 0, sd = 0; bool ok = false;
				if (i - 1 >= 0)
				{
					const int upto = i - 1, lo = upto - Z;
					const double s1 = I.rsPre1[k][upto] - (lo >= 0 ? I.rsPre1[k][lo] : 0.0), s2 = I.rsPre2[k][upto] - (lo >= 0 ? I.rsPre2[k][lo] : 0.0);
					const int cnt = upto - Max(lo, -1);
					if (cnt >= 10) { mean = s1 / cnt; const double var = s2 / cnt - mean * mean; sd = var > 0 ? sqrt(var) : 0; ok = sd > 0; }
				}
				const float z = ok ? Clamp1((rs - mean) / sd / 2.0) : 0.0f;
				rsOut[k] = z; rsSum += z; ++rsCnt;
				if (closed) { I.rsPre1[k][i] = (i > 0 ? I.rsPre1[k][i - 1] : 0.0) + rs; I.rsPre2[k][i] = (i > 0 ? I.rsPre2[k][i - 1] : 0.0) + rs * rs; }
			}
			I.rsYM[i] = IsNan(rsOut[0]) ? 0.0f : rsOut[0]; I.rsES[i] = IsNan(rsOut[1]) ? 0.0f : rsOut[1]; I.rsRTY[i] = IsNan(rsOut[2]) ? 0.0f : rsOut[2];
			I.rsIndex[i] = rsCnt > 0 ? rsSum / rsCnt : kNaN;

			// ---- SMT divergence at a freshly confirmed primary swing ----
			float smtNow = 0;
			if (closed && !A.swings.empty() && A.swings.back().confirmIdx == i && rsCnt > 0)
			{
				const Swing& ns = A.swings.back();
				const int prevIdx = ns.high ? A.prevSwingHigh : A.prevSwingLow;
				if (prevIdx >= 0 && prevIdx < static_cast<int>(A.swings.size()) - 1)
				{
					const Swing& ps = A.swings[prevIdx];
					const int N = Max(1, S.params.auction.swingStrength);
					int diverging = 0, checked = 0;
					for (int k = 0; k < 3; ++k)
					{
						RefChart& R = *idx[k]; if (!R.ok) continue;
						const int jn0 = Align(sc, R, Max(0, ns.idx - N), n), jn1 = Align(sc, R, Min(n - 1, ns.idx + N), n);
						const int jp0 = Align(sc, R, Max(0, ps.idx - N), n), jp1 = Align(sc, R, Min(n - 1, ps.idx + N), n);
						if (jn0 < 0 || jn1 < 0 || jp0 < 0 || jp1 < 0) continue;
						++checked;
						if (ns.high && ns.price > ps.price && RefHighBetween(R, jn0, jn1, true) <= RefHighBetween(R, jp0, jp1, true)) ++diverging;
						if (!ns.high && ns.price < ps.price && RefHighBetween(R, jn0, jn1, false) >= RefHighBetween(R, jp0, jp1, false)) ++diverging;
					}
					if (checked > 0 && diverging > 0)
					{
						smtNow = (ns.high ? -1.0f : 1.0f) * static_cast<float>(diverging) / checked;
						I.smtMark[i] = ns.high ? -1 : 1; I.lastSmtIdx = i; I.lastSmtVal = smtNow; I.lastSmtPrice = ns.price;
						{ char t[72]; sprintf_s(t, sizeof(t), "SMT: %s %s, index no %s", sc.Symbol.GetChars(), ns.high ? "HH" : "LL", ns.high ? "HH" : "LL"); PushEvent(sc, S, ns.idx, EV_SMT, ns.high ? -1 : 1, ns.price, t); }
					}
				}
			}
			I.smt[i] = (rsCnt > 0 && I.lastSmtIdx >= 0 && i >= I.lastSmtIdx) ? I.lastSmtVal * static_cast<float>(exp(-(i - I.lastSmtIdx) / 16.0)) : (rsCnt > 0 ? 0.0f : kNaN);

			// ---- NYSE TICK ----
			if (I.tick.ok)
			{
				const int j = Align(sc, I.tick, i, n);
				const float tv = j >= 0 ? I.tick.data[SC_LAST][j] : 0.0f;
				I.tickVal[i] = tv;
				const int key = B.isRth[i] ? B.rthSession[i] : -1;
				if (key != I.tickSession) { I.tickCumCommitted = 0; I.tickCount = 0; I.tickSession = key; }
				const double cum = I.tickCumCommitted + tv; const int cnt = I.tickCount + 1;
				if (closed) { I.tickCumCommitted = cum; I.tickCount = cnt; }
				I.tickCum[i] = (key >= 0 && cnt > 0) ? Clamp1((cum / cnt) / 400.0) : 0.0f;
				// extremes in the lookback
				int pos = 0, neg = 0; const int LB = Max(1, P.tickLookback);
				for (int q = i; q > i - LB && q >= 0; --q)
				{
					const float x = I.tickVal[q];
					if (x >= P.tickStrong) pos += 2; else if (x >= P.tickExtreme) pos += 1;
					if (x <= -P.tickStrong) neg += 2; else if (x <= -P.tickExtreme) neg += 1;
				}
				I.tickExt[i] = Clamp1(static_cast<double>(pos - neg) / LB);
				// trend divergence: TICK EMA slope vs price slope
				const float a = 2.0f / (Max(2, P.tickEma) + 1.0f);
				const float ema = (i == 0) ? tv : I.tickEmaArr[i - 1] + a * (tv - I.tickEmaArr[i - 1]);
				I.tickEmaArr[i] = ema;
				const int k = Max(2, P.tickEma);
				if (i - k >= 0 && atr > 0)
				{
					const float ps = Sign(c - sc.Close[i - k]), ts = (fabs(ema - I.tickEmaArr[i - k]) >= 50.0f) ? Sign(ema - I.tickEmaArr[i - k]) : 0.0f;
					I.tickDiv[i] = (ps != 0 && ts != 0 && ps != ts) ? ts : 0.0f;
				}
				else I.tickDiv[i] = 0;
			}
			else { I.tickVal[i] = 0; I.tickEmaArr[i] = 0; I.tickCum[i] = kNaN; I.tickExt[i] = kNaN; I.tickDiv[i] = kNaN; }

			// ---- mega-cap leadership ----
			if (megaN > 0)
			{
				float sum = 0; int cnt = 0;
				for (int k = 0; k < 6; ++k)
				{
					RefChart& R = I.mega[k]; if (!R.ok) { I.megaState[k] = 0; continue; }
					const int j = Align(sc, R, i, n); if (j < 0) { I.megaState[k] = 0; continue; }
					const float rc = R.data[SC_LAST][j];
					float s = 0;
					if (R.vwap[j] > 0) s += rc > R.vwap[j] ? 0.5f : (rc < R.vwap[j] ? -0.5f : 0.0f);
					s += rc > R.ema[j] ? 0.5f : (rc < R.ema[j] ? -0.5f : 0.0f);
					I.megaState[k] = static_cast<signed char>(s >= 0.5f ? 1 : (s <= -0.5f ? -1 : 0));
					sum += s; ++cnt;
				}
				I.megaCap[i] = cnt > 0 ? Clamp1(sum / cnt) : kNaN;
			}
			else I.megaCap[i] = kNaN;

			// ---- lead / lag vs YM and the first two mega caps (rolling correlation at lags -3..+3) ----
			{
				I.retP[i] = (i > 0 && sc.Close[i - 1] > 0) ? c / sc.Close[i - 1] - 1.0f : 0.0f;
				RefChart* lr[3] = { &I.ym, nullptr, nullptr }; int lc = 1;
				for (int k = 0; k < 6 && lc < 3; ++k) if (I.mega[k].ok) lr[lc++] = &I.mega[k];
				float best = 0; int bestLag = 0, bestRef = -1; float feature = 0;
				for (int r = 0; r < 3; ++r)
				{
					RefChart* R = lr[r];
					if (R == nullptr || !R->ok) { I.retR[r][i] = 0; continue; }
					const int j = Align(sc, *R, i, n);
					I.retR[r][i] = (j > 0 && R->data[SC_LAST][j - 1] > 0) ? R->data[SC_LAST][j] / R->data[SC_LAST][j - 1] - 1.0f : 0.0f;
					if (i < 70) continue;
					const int W = 60;
					for (int lag = -3; lag <= 3; ++lag)
					{
						double sxy = 0, sxx = 0, syy = 0, sx = 0, sy = 0; int m = 0;
						for (int q = i - W + 1; q <= i; ++q)
						{
							const int qr = q - lag; if (qr < 0 || qr > i) continue;
							const double x = I.retP[q], y = I.retR[r][qr];
							sx += x; sy += y; sxx += x * x; syy += y * y; sxy += x * y; ++m;
						}
						if (m < 20) continue;
						const double cov = sxy / m - (sx / m) * (sy / m), vx = sxx / m - (sx / m) * (sx / m), vy = syy / m - (sy / m) * (sy / m);
						const double corr = (vx > 0 && vy > 0) ? cov / sqrt(vx * vy) : 0.0;
						if (fabs(corr) > fabs(best)) { best = static_cast<float>(corr); bestLag = lag; bestRef = r; }
					}
				}
				if (bestRef >= 0 && bestLag > 0 && fabs(best) >= 0.3f)
				{
					// the reference leads by bestLag bars: has it moved while the primary has not?
					double rsum = 0, psum = 0, rsq = 0, psq = 0; const int W = 60;
					for (int q = Max(1, i - W + 1); q <= i; ++q) { rsq += static_cast<double>(I.retR[bestRef][q]) * I.retR[bestRef][q]; psq += static_cast<double>(I.retP[q]) * I.retP[q]; }
					const double rsd = sqrt(rsq / W), psd = sqrt(psq / W);
					for (int q = i - bestLag + 1; q <= i; ++q) { rsum += I.retR[bestRef][q]; psum += I.retP[q]; }
					const double zr = rsd > 0 ? rsum / (rsd * sqrt(static_cast<double>(bestLag))) : 0, zp = psd > 0 ? psum / (psd * sqrt(static_cast<double>(bestLag))) : 0;
					if (fabs(zr) >= 1.0 && fabs(zp) < 0.5) feature = Clamp1(Sign(zr) * Min(1.0, fabs(zr) / 2.0));
					if (closed) { I.leadBars = bestLag; I.leadCorr = best; I.leadRef = bestRef; sprintf_s(I.leadText, sizeof(I.leadText), "%s leads %db%s", bestRef == 0 ? "YM" : (lr[bestRef] && !lr[bestRef]->symbol.IsEmpty() ? lr[bestRef]->symbol.GetChars() : "cap"), bestLag, feature > 0 ? " ^" : (feature < 0 ? " v" : "")); }
				}
				else if (closed) { I.leadBars = 0; I.leadRef = -1; I.leadText[0] = 0; }
				I.leadLag[i] = (I.ym.ok || megaN > 0) ? feature : kNaN;
			}
			// composite for the subgraph (mean of what is available)
			{
				float s = 0; int cnt = 0;
				const float parts[4] = { I.rsIndex[i], I.tickCum[i], I.tickExt[i], I.megaCap[i] };
				for (int k = 0; k < 4; ++k) if (!IsNan(parts[k])) { s += parts[k]; ++cnt; }
				I.composite[i] = cnt > 0 ? s / cnt : 0.0f;
			}
		}
		I.Stamp(sc);
	}

	// ==== 11 Weights + DCS composite + setups ===================================
	namespace dcs_detail
	{
		inline std::string Trim(const std::string& x)
		{
			size_t a = 0, b = x.size();
			while (a < b && (x[a] == ' ' || x[a] == '\t' || x[a] == '\r' || x[a] == '\n')) ++a;
			while (b > a && (x[b - 1] == ' ' || x[b - 1] == '\t' || x[b - 1] == '\r' || x[b - 1] == '\n')) --b;
			return x.substr(a, b - a);
		}

		// Parses "key = value" lines into the weights. Returns false when the file cannot be opened.
		bool ParseWeightsFile(const char* path, Weights& w)
		{
			FILE* f = fopen(path, "r");
			if (!f) return false;
			char line[512];
			while (fgets(line, sizeof(line), f))
			{
				std::string s(line);
				const size_t hash = s.find('#'); if (hash != std::string::npos) s = s.substr(0, hash);
				const size_t eq = s.find('='); if (eq == std::string::npos) continue;
				const std::string key = Trim(s.substr(0, eq)); const std::string val = Trim(s.substr(eq + 1));
				if (key.empty() || val.empty()) continue;
				const double v = atof(val.c_str());
				if (key == "version") w.version = static_cast<int>(v);
				else if (key == "thr.signal") w.thrSignal = static_cast<float>(v);
				else if (key == "thr.fade") w.thrFade = static_cast<float>(v);
				else if (key.compare(0, 2, "w.") == 0)
				{
					const std::string name = key.substr(2);
					for (int k = 0; k < F_COUNT; ++k) if (name == kFeatureNames[k]) { w.w[k] = static_cast<float>(v); break; }
				}
				else if (key.compare(0, 5, "gate.") == 0)
				{
					const size_t dot = key.find('.', 5); if (dot == std::string::npos) continue;
					const std::string reg = key.substr(5, dot - 5), grp = key.substr(dot + 1);
					int g = -1; for (int k = 0; k < G_COUNT; ++k) if (grp == kGroupNames[k]) g = k;
					if (g < 0) continue;
					if (reg == "trend") { w.gate[RG_TREND_UP][g] = static_cast<float>(v); w.gate[RG_TREND_DOWN][g] = static_cast<float>(v); }
					else if (reg == "balance") w.gate[RG_BALANCE][g] = static_cast<float>(v);
					else if (reg == "chop") w.gate[RG_CHOP][g] = static_cast<float>(v);
				}
			}
			fclose(f);
			return true;
		}

		// Hot reload: re-read when the file's mtime changed. Returns true when weights changed.
		bool MaybeReloadWeights(SCStudyInterfaceRef sc, ChartState& S)
		{
			DcsState& D = S.dcs; const DcsParams& P = S.params.dcs;
			const double now = static_cast<double>(GetTickCount64()) / 1000.0;
			if (D.weights.lastCheck > 0 && now - D.weights.lastCheck < Max(1, P.hotReloadSec)) return false;
			D.weights.lastCheck = now;
			SCString path = sc.DataFilesFolder();
			if (path.GetLength() > 0 && path.GetChars()[path.GetLength() - 1] != '\\' && path.GetChars()[path.GetLength() - 1] != '/') path += "\\";
			path += P.weightsFile;
			struct _stat st;
			if (_stat(path.GetChars(), &st) != 0) { if (D.weights.loadedFromFile) { D.weights.SetDefaults(); D.weights.loadedFromFile = false; D.weights.fileMtime = 0; return true; } return false; }
			if (D.weights.loadedFromFile && st.st_mtime == D.weights.fileMtime) return false;
			Weights fresh; fresh.SetDefaults();
			if (!ParseWeightsFile(path.GetChars(), fresh)) return false;
			fresh.fileMtime = st.st_mtime; fresh.loadedFromFile = true; fresh.lastCheck = now;
			const bool changed = std::memcmp(fresh.w, D.weights.w, sizeof(fresh.w)) != 0 || std::memcmp(fresh.gate, D.weights.gate, sizeof(fresh.gate)) != 0
				|| fresh.thrSignal != D.weights.thrSignal || fresh.thrFade != D.weights.thrFade || !D.weights.loadedFromFile;
			D.weights = fresh;
			if (changed) { SCString m; m.Format("NQ Edge: weights loaded from %s (version %d)", path.GetChars(), fresh.version); sc.AddMessageToLog(m, 0); }
			return changed;
		}

		// ---- level list as of bar i (for targets, location tests, HUD) ----
		struct Lv { float price; int kind; };
		void GatherLevels(SCStudyInterfaceRef sc, ChartState& S, int i, std::vector<Lv>& out)
		{
			out.clear();
			const AuctionState& A = S.auction; const VwapState& V = S.vwap; const FlowState& F = S.flow; const AuctionParams& AP = S.params.auction;
			if (A.poc[i] > 0) { out.push_back({ A.poc[i], LVL_POC }); out.push_back({ A.vah[i], LVL_VAH }); out.push_back({ A.val[i], LVL_VAL }); }
			if (A.pdPoc[i] > 0) { out.push_back({ A.pdPoc[i], LVL_PD_POC }); out.push_back({ A.pdVah[i], LVL_PD_VAH }); out.push_back({ A.pdVal[i], LVL_PD_VAL }); }
			if (A.pdh[i] > 0) { out.push_back({ A.pdh[i], LVL_PDH }); out.push_back({ A.pdl[i], LVL_PDL }); }
			if (i < static_cast<int>(A.pwh.size()) && A.pwh[i] > 0) { out.push_back({ A.pwh[i], LVL_PWH }); out.push_back({ A.pwl[i], LVL_PWL }); }
			if (i < static_cast<int>(A.pdc.size()) && A.pdc[i] > 0) out.push_back({ A.pdc[i], LVL_PDC });
			if (i < static_cast<int>(A.sessOpen.size()) && A.sessOpen[i] > 0) out.push_back({ A.sessOpen[i], LVL_OPEN });
			if (A.onHigh[i] > 0) { out.push_back({ A.onHigh[i], LVL_ONH }); out.push_back({ A.onLow[i], LVL_ONL }); }
			if (A.ibHigh[i] > 0 && A.ibDone[i])
			{
				const float r = A.ibHigh[i] - A.ibLow[i];
				out.push_back({ A.ibHigh[i], LVL_IBH }); out.push_back({ A.ibLow[i], LVL_IBL });
				const float m[3] = { AP.ibExtA, AP.ibExtB, AP.ibExtC };
				for (int k = 0; k < 3; ++k) if (m[k] > 0) { out.push_back({ A.ibHigh[i] + r * m[k], LVL_IBEXT }); out.push_back({ A.ibLow[i] - r * m[k], LVL_IBEXT }); }
			}
			if (V.vwap[i] > 0)
			{
				out.push_back({ V.vwap[i], LVL_VWAP }); out.push_back({ V.b1u[i], LVL_VWAP_B1U }); out.push_back({ V.b1d[i], LVL_VWAP_B1D });
				out.push_back({ V.b2u[i], LVL_VWAP_B2U }); out.push_back({ V.b2d[i], LVL_VWAP_B2D }); out.push_back({ V.b3u[i], LVL_VWAP_B3U }); out.push_back({ V.b3d[i], LVL_VWAP_B3D });
			}
			for (size_t k = 0; k < A.nakedHist.size(); ++k) { const AuctionState::NakedRec& r = A.nakedHist[k]; if (r.born <= i && (r.dead < 0 || r.dead > i)) out.push_back({ r.price, LVL_NAKED_POC }); }
			// last confirmed swings as of i
			bool gotH = false, gotL = false;
			for (int k = static_cast<int>(A.swings.size()) - 1; k >= 0 && !(gotH && gotL); --k)
			{
				const Swing& s = A.swings[k]; if (s.confirmIdx > i) continue;
				if (s.high && !gotH) { out.push_back({ s.price, LVL_SWING_H }); gotH = true; }
				if (!s.high && !gotL) { out.push_back({ s.price, LVL_SWING_L }); gotL = true; }
			}
			for (size_t z = 0; z < A.liquidity.size(); ++z) { const Zone& Z = A.liquidity[z]; if (Z.bornIdx <= i && (Z.active || Z.deadIdx > i)) out.push_back({ 0.5f * (Z.top + Z.bottom), Z.kind }); }
			for (size_t z = 0; z < F.absorbZones.size(); ++z) { const Zone& Z = F.absorbZones[z]; if (Z.bornIdx < i && (Z.active || Z.deadIdx > i)) out.push_back({ Z.dir > 0 ? Z.bottom : Z.top, LVL_ABSORB }); }
			for (size_t z = 0; z < F.imbZones.size(); ++z) { const Zone& Z = F.imbZones[z]; if (Z.bornIdx < i && (Z.active || Z.deadIdx > i)) out.push_back({ Z.dir > 0 ? Z.bottom : Z.top, LVL_IMB }); }
		}

		inline const char* LevelName(int kind)
		{
			switch (kind)
			{
			case LVL_POC: return "POC"; case LVL_VAH: return "VAH"; case LVL_VAL: return "VAL"; case LVL_PD_POC: return "pdPOC"; case LVL_PD_VAH: return "pdVAH"; case LVL_PD_VAL: return "pdVAL";
			case LVL_PDH: return "PDH"; case LVL_PDL: return "PDL"; case LVL_ONH: return "ONH"; case LVL_ONL: return "ONL"; case LVL_IBH: return "IBH"; case LVL_IBL: return "IBL"; case LVL_IBEXT: return "IBext";
			case LVL_NAKED_POC: return "nPOC"; case LVL_SWING_H: return "swing H"; case LVL_SWING_L: return "swing L"; case LVL_LIQ_EQH: return "EQH"; case LVL_LIQ_EQL: return "EQL";
			case LVL_VWAP: return "VWAP"; case LVL_VWAP_B1U: return "VWAP+1"; case LVL_VWAP_B1D: return "VWAP-1"; case LVL_VWAP_B2U: return "VWAP+2"; case LVL_VWAP_B2D: return "VWAP-2";
			case LVL_VWAP_B3U: return "VWAP+3"; case LVL_VWAP_B3D: return "VWAP-3"; case LVL_ABSORB: return "absorb"; case LVL_IMB: return "imb"; case LVL_SINGLE_PRINT: return "single";
			case LVL_PDC: return "PDC"; case LVL_OPEN: return "OPEN"; case LVL_FAILED: return "fail"; case LVL_PWH: return "PWH"; case LVL_PWL: return "PWL";
			default: return "level";
			}
		}

		// nearest level of one of the given kinds within tol of price (kinds terminated by -1)
		inline bool NearLevel(const std::vector<Lv>& lv, float price, float tol, const int* kinds, float& r_price, int& r_kind)
		{
			float best = FLT_MAX; bool found = false;
			for (size_t k = 0; k < lv.size(); ++k)
			{
				bool ok = false; for (int q = 0; kinds[q] >= 0; ++q) if (lv[k].kind == kinds[q]) { ok = true; break; }
				if (!ok) continue;
				const float d = static_cast<float>(fabs(lv[k].price - price));
				if (d <= tol && d < best) { best = d; r_price = lv[k].price; r_kind = lv[k].kind; found = true; }
			}
			return found;
		}

		// targets: nearest levels beyond entry in the trade direction, at least minDist away
		inline bool PickTargets(const std::vector<Lv>& lv, int dir, float entry, float minDist, float& t1, float& t2, int& k1, int& k2)
		{
			float b1 = FLT_MAX, b2 = FLT_MAX; k1 = k2 = LVL_NONE; t1 = t2 = 0;
			for (size_t k = 0; k < lv.size(); ++k)
			{
				const float d = dir > 0 ? lv[k].price - entry : entry - lv[k].price;
				if (d < minDist) continue;
				if (d < b1) { b2 = b1; t2 = t1; k2 = k1; b1 = d; t1 = lv[k].price; k1 = lv[k].kind; }
				else if (d < b2 && d > b1) { b2 = d; t2 = lv[k].price; k2 = lv[k].kind; }
			}
			if (b1 == FLT_MAX) return false;
			if (b2 == FLT_MAX) { t2 = entry + (t1 - entry) * 2.0f; k2 = LVL_NONE; }
			return true;
		}

		// contracts for the configured $ risk (0 when the symbol's currency value per tick is unknown)
		inline int RiskSize(SCStudyInterfaceRef sc, const ChartState& S, float riskPts)
		{
			const double cv = sc.CurrencyValuePerTick; const double rk = S.params.dcs.riskPerTrade;
			if (cv <= 0 || riskPts <= 0 || rk <= 0 || S.tickSize <= 0) return 0;
			return static_cast<int>(floor(rk / ((riskPts / S.tickSize) * cv)));
		}
		// how many feature groups lean in `dir` (from the per-bar group sign bitmasks)
		inline void GroupAgreement(const DcsState& D, int i, int dir, int& agree, int& total)
		{
			agree = 0; total = 0; if (i < 0 || i >= static_cast<int>(D.gAvail.size())) return;
			for (int g = 0; g < G_COUNT; ++g)
			{
				if (!((D.gAvail[i] >> g) & 1)) continue;
				++total; const bool pos = ((D.gPos[i] >> g) & 1) != 0;
				if ((dir > 0 && pos) || (dir < 0 && !pos)) ++agree;
			}
		}

		struct Candidate { int type; int dir; float stop; float refLevel; int refKind; int score; float trigger; };

		// Confluence grade: bias + location quality + trigger strength + intermarket agreement + regime fit -> A/B/C
		int GradeSignal(ChartState& S, int i, const Candidate& cd, float dcs, float thr)
		{
			const InterState& I = S.inter; const RegimeState& R = S.regime;
			float score = 0;
			const float ad = static_cast<float>(fabs(dcs));
			score += ad >= thr ? 1.0f : (ad >= 0.5f * thr ? 0.5f : 0.0f);
			switch (cd.refKind)
			{
			case LVL_VWAP: case LVL_POC: case LVL_VAH: case LVL_VAL: case LVL_PDH: case LVL_PDL: case LVL_IBH: case LVL_IBL: case LVL_NAKED_POC: case LVL_LIQ_EQH: case LVL_LIQ_EQL: case LVL_ABSORB: case LVL_PWH: case LVL_PWL: score += 1.0f; break;
			default: score += 0.5f; break;
			}
			score += Min(1.5f, cd.trigger);
			{
				float s = 0; int n = 0;
				const float parts[3] = { I.rsIndex[i], I.megaCap[i], I.tickCum[i] };
				for (int k = 0; k < 3; ++k) if (!IsNan(parts[k])) { s += parts[k]; ++n; }
				if (n == 0) score += 0.5f; else { const float m = s / n; score += (m * cd.dir > 0.1f) ? 1.0f : (m * cd.dir < -0.1f ? -0.5f : 0.25f); }
			}
			{
				int ag = 0, tot = 0; GroupAgreement(S.dcs, i, cd.dir, ag, tot);
				if (tot >= 3) { const float fr = static_cast<float>(ag) / tot; score += fr >= 0.75f ? 0.5f : (fr < 0.5f ? -0.5f : 0.0f); }
			}
			const int reg = R.regime[i];
			switch (cd.type)
			{
			case SETUP_TREND_PULLBACK: score += (reg == RG_TREND_UP || reg == RG_TREND_DOWN) ? 1.0f : 0.25f; break;
			case SETUP_VALUE_EDGE: score += reg == RG_BALANCE ? 1.0f : 0.25f; break;
			case SETUP_FAILED_BREAKOUT: score += 0.75f; break;
			case SETUP_BREAK_ACCEPT: score += (reg == RG_TREND_UP || reg == RG_TREND_DOWN) ? 1.0f : 0.6f; break;
			case SETUP_DIVERGENCE: score += reg == RG_BALANCE ? 1.0f : 0.5f; break;
			default: break;
			}
			return score >= 4.0f ? 1 : (score >= 3.0f ? 2 : 3);
		}

		bool BuildSignal(SCStudyInterfaceRef sc, ChartState& S, int i, const Candidate& cd, const std::vector<Lv>& lv, Signal& out)
		{
			const DcsParams& P = S.params.dcs; const float atr = AtrAt(S, i);
			const float entry = sc.Close[i];
			float stop = cd.stop;
			if (cd.dir > 0 && stop >= entry) return false;
			if (cd.dir < 0 && stop <= entry) return false;
			// stop sanity: a structural stop that is too tight gets run over by noise, one that is too wide kills the R:R
			const float minRisk = Max(P.minStopAtr * atr, P.minStopTicks * S.tickSize);
			if (cd.dir > 0 && entry - stop < minRisk) stop = entry - minRisk;
			if (cd.dir < 0 && stop - entry < minRisk) stop = entry + minRisk;
			if (fabs(entry - stop) > P.maxStopAtr * atr) return false;
			const float risk = static_cast<float>(fabs(entry - stop));
			if (risk < S.tickSize) return false;
			float t1, t2; int k1, k2;
			if (!PickTargets(lv, cd.dir, entry, Max(P.minTargetAtr * atr, risk * P.minRR), t1, t2, k1, k2)) return false;
			const float rr = static_cast<float>(fabs(t1 - entry)) / risk;
			if (rr < P.minRR) return false;
			out = Signal();
			out.idx = i; out.type = cd.type; out.dir = cd.dir; out.entry = entry; out.stop = sc.RoundToTickSize(stop); out.t1 = sc.RoundToTickSize(t1); out.t2 = sc.RoundToTickSize(t2);
			out.dcs = S.dcs.dcs[i]; out.rr = rr; out.size = RiskSize(sc, S, risk);
			sprintf_s(out.label, sizeof(out.label), "%s | %s | DCS %+.0f | R:R %.1f | T1 %s", cd.dir > 0 ? "LONG" : "SHORT", kSetupNames[cd.type], out.dcs, rr, LevelName(k1));
			return true;
		}
	}

	void EnsureDcs(SCStudyInterfaceRef sc, ChartState& S)
	{
		using namespace dcs_detail;
		EnsureInter(sc, S);
		const int n = sc.ArraySize; if (n <= 0) return;
		DcsState& D = S.dcs; const DcsParams& P = S.params.dcs;
		if (MaybeReloadWeights(sc, S)) { ResetFrom(S, E_DCS); }
		Fit(D.feat, n * F_COUNT); Fit(D.dcs, n); Fit(D.dcsSmooth, n); Fit(D.barState, n); Fit(D.gPos, n); Fit(D.gAvail, n); Fit(D.signalType, n); Fit(D.signalDir, n);
		if (D.UpToDate(sc)) return;
		int from = D.computedThrough + 1; if (from < 0) from = 0; if (from > n - 1) from = n - 1;
		const AuctionState& A = S.auction; const VwapState& V = S.vwap; const FlowState& F = S.flow; const RegimeState& R = S.regime; const InterState& I = S.inter;
		const Weights& W = D.weights;
		const float thr = P.signalThr > 0 ? P.signalThr : W.thrSignal;
		const float fadeThr = P.fadeThr > 0 ? P.fadeThr : W.thrFade;
		const float alpha = 2.0f / (Max(1, P.smoothLen) + 1.0f);
		std::vector<Lv> lv; lv.reserve(64);

		for (int i = from; i < n; ++i)
		{
			const bool closed = (i <= n - 2);
			float* f = &D.feat[static_cast<size_t>(i) * F_COUNT];
			f[F_VWAP_POS] = V.pos[i]; f[F_VWAP_SLOPE] = V.slope[i]; f[F_VWAP_ACCEPT] = V.accept[i];
			f[F_STRUCT_TREND] = A.structTrend[i]; f[F_BOS] = A.bos[i]; f[F_VA_POS] = A.vaPos[i]; f[F_POC_POS] = A.pocPos[i]; f[F_IB_POS] = A.ibPos[i];
			f[F_VALUE_MIG] = A.valueMig[i]; f[F_OPEN_TYPE] = A.openTypeDir[i];
			f[F_DELTA] = Clamp1(F.deltaPct[i] / 0.3); f[F_CVD_Z] = Clamp1(F.cvdZ[i] / 2.0); f[F_CVD_DIV] = F.fCvdDiv[i]; f[F_ABSORB] = F.fAbsorb[i]; f[F_EXHAUST] = F.fExhaust[i];
			f[F_IMBALANCE] = F.fImb[i]; f[F_TRAPPED] = F.fTrapped[i]; f[F_LARGE_TRADE] = F.fLarge[i];
			f[F_REGIME_TREND] = R.regimeTrend[i]; f[F_MTF_BIAS] = R.mtfBias[i];
			f[F_RS_INDEX] = I.rsIndex[i]; f[F_SMT] = I.smt[i]; f[F_TICK_CUM] = I.tickCum[i]; f[F_TICK_EXT] = I.tickExt[i]; f[F_TICK_DIV] = I.tickDiv[i]; f[F_MEGA_CAP] = I.megaCap[i];
			f[F_LEG_EFF] = F.fLegEff[i]; f[F_LEG_VOL] = F.fLegVol[i]; f[F_PULLBACK] = F.fPullback[i]; f[F_ABSORB_Q] = F.fAbsorbQ[i]; f[F_AUCTION] = A.auctionF[i]; f[F_LEADLAG] = I.leadLag[i];

			const int reg = Clamp(static_cast<int>(R.regime[i]), 0, 4);
			double num = 0, den = 0; double gs[G_COUNT] = { 0, 0, 0, 0, 0, 0 }; bool ga[G_COUNT] = { false, false, false, false, false, false };
			for (int k = 0; k < F_COUNT; ++k)
			{
				if (IsNan(f[k])) continue;
				const float g = (reg == RG_NONE) ? 1.0f : W.gate[reg][kFeatureGroup[k]];
				const float wk = W.w[k] * g;
				num += wk * f[k]; den += fabs(wk);
				gs[kFeatureGroup[k]] += wk * f[k]; ga[kFeatureGroup[k]] = true;
			}
			const float dcs = den > 0 ? static_cast<float>(Clamp(100.0 * num / den, -100.0, 100.0)) : 0.0f;
			D.dcs[i] = dcs;
			{
				unsigned char pos = 0, av = 0;
				for (int g = 0; g < G_COUNT; ++g) { if (!ga[g] || fabs(gs[g]) < 1e-6) continue; av |= static_cast<unsigned char>(1 << g); if (gs[g] > 0) pos |= static_cast<unsigned char>(1 << g); }
				D.gPos[i] = pos; D.gAvail[i] = av;
			}
			D.dcsSmooth[i] = (i == 0) ? dcs : D.dcsSmooth[i - 1] + alpha * (dcs - D.dcsSmooth[i - 1]);
			D.barState[i] = static_cast<signed char>(dcs >= P.strongThr ? 2 : (dcs >= P.weakThr ? 1 : (dcs <= -P.strongThr ? -2 : (dcs <= -P.weakThr ? -1 : 0))));
			D.signalType[i] = 0; D.signalDir[i] = 0;

			// ---- setups: closed bars only, one signal per bar ----
			if (!closed || i < 30 || i <= D.lastSignalCheckedIdx) continue;
			D.lastSignalCheckedIdx = i;
			const float atr = AtrAt(S, i); if (atr <= 0) continue;
			const float c = sc.Close[i], h = sc.High[i], l = sc.Low[i];
			const float tol = P.levelTolAtr * atr, buf = P.stopBufferAtr * atr;
			const float mtf = R.mtfBias[i];
			GatherLevels(sc, S, i, lv);
			std::vector<Candidate> cands;
			const float dPct = F.deltaPct[i], dPctPrev = i > 0 ? F.deltaPct[i - 1] : 0.0f;
			const bool deltaFlipUp = dPct >= 0.15f && dPctPrev < 0, deltaFlipDn = dPct <= -0.15f && dPctPrev > 0;
			const bool imbBuy = F.imbMark[i] == 1 || F.imbMark[i] == 2, imbSell = F.imbMark[i] == -1 || F.imbMark[i] == 2;

			// 1. Trend pullback continuation
			if (P.setupOn[SETUP_TREND_PULLBACK])
			{
				static const int kinds[] = { LVL_VWAP, LVL_VWAP_B1D, LVL_VWAP_B1U, LVL_POC, LVL_ABSORB, -1 };
				float lp; int lk;
				if (reg == RG_TREND_UP && dcs >= thr && (!P.requireMtf || mtf > 0.1f) && NearLevel(lv, l, tol, kinds, lp, lk) && c > lp && (F.absorbMark[i] == 1 || deltaFlipUp || imbBuy))
					cands.push_back({ SETUP_TREND_PULLBACK, 1, Min(l, lp) - buf, lp, lk, 3, (F.absorbMark[i] == 1 ? 1.0f : 0.0f) + (imbBuy ? 1.0f : 0.0f) + (deltaFlipUp ? 0.5f : 0.0f) });
				if (reg == RG_TREND_DOWN && dcs <= -thr && (!P.requireMtf || mtf < -0.1f) && NearLevel(lv, h, tol, kinds, lp, lk) && c < lp && (F.absorbMark[i] == -1 || deltaFlipDn || imbSell))
					cands.push_back({ SETUP_TREND_PULLBACK, -1, Max(h, lp) + buf, lp, lk, 3, (F.absorbMark[i] == -1 ? 1.0f : 0.0f) + (imbSell ? 1.0f : 0.0f) + (deltaFlipDn ? 0.5f : 0.0f) });
			}
			// 2. Value-edge rejection in balance
			if (P.setupOn[SETUP_VALUE_EDGE] && reg == RG_BALANCE)
			{
				static const int hiKinds[] = { LVL_VAH, LVL_PD_VAH, LVL_IBH, LVL_VWAP_B2U, -1 };
				static const int loKinds[] = { LVL_VAL, LVL_PD_VAL, LVL_IBL, LVL_VWAP_B2D, -1 };
				float lp; int lk;
				if (dcs <= -fadeThr * 0.5f && NearLevel(lv, h, tol, hiKinds, lp, lk) && c < lp && (F.absorbMark[i] == -1 || F.exhaustMark[i] == -1 || F.trapMark[i] == -1 || (dPct <= -0.15f && h >= lp)))
					cands.push_back({ SETUP_VALUE_EDGE, -1, Max(h, lp) + buf, lp, lk, 2, (F.absorbMark[i] == -1 ? 1.0f : 0.0f) + (F.exhaustMark[i] == -1 ? 0.75f : 0.0f) + (F.trapMark[i] == -1 ? 1.0f : 0.0f) + (dPct <= -0.15f ? 0.5f : 0.0f) });
				if (dcs >= fadeThr * 0.5f && NearLevel(lv, l, tol, loKinds, lp, lk) && c > lp && (F.absorbMark[i] == 1 || F.exhaustMark[i] == 1 || F.trapMark[i] == 1 || (dPct >= 0.15f && l <= lp)))
					cands.push_back({ SETUP_VALUE_EDGE, 1, Min(l, lp) - buf, lp, lk, 2, (F.absorbMark[i] == 1 ? 1.0f : 0.0f) + (F.exhaustMark[i] == 1 ? 0.75f : 0.0f) + (F.trapMark[i] == 1 ? 1.0f : 0.0f) + (dPct >= 0.15f ? 0.5f : 0.0f) });
			}
			// 3. Failed breakout / trapped traders
			if (P.setupOn[SETUP_FAILED_BREAKOUT] && F.trapMark[i] != 0)
			{
				static const int hiKinds[] = { LVL_IBH, LVL_PDH, LVL_VAH, LVL_ONH, LVL_SWING_H, LVL_LIQ_EQH, LVL_PD_VAH, LVL_PWH, -1 };
				static const int loKinds[] = { LVL_IBL, LVL_PDL, LVL_VAL, LVL_ONL, LVL_SWING_L, LVL_LIQ_EQL, LVL_PD_VAL, LVL_PWL, -1 };
				const int K = Max(1, S.params.flow.trapReversalBars) + 1;
				float ext = F.trapMark[i] < 0 ? -FLT_MAX : FLT_MAX;
				for (int q = Max(0, i - K); q <= i; ++q) ext = F.trapMark[i] < 0 ? Max(ext, sc.High[q]) : Min(ext, sc.Low[q]);
				float lp; int lk;
				if (F.trapMark[i] < 0 && dcs < thr && NearLevel(lv, ext, tol * 1.5f, hiKinds, lp, lk)) cands.push_back({ SETUP_FAILED_BREAKOUT, -1, ext + buf, lp, lk, 4, 1.0f + (F.absorbMark[i] == -1 ? 0.5f : 0.0f) + (dPct <= -0.15f ? 0.5f : 0.0f) });
				if (F.trapMark[i] > 0 && dcs > -thr && NearLevel(lv, ext, tol * 1.5f, loKinds, lp, lk)) cands.push_back({ SETUP_FAILED_BREAKOUT, 1, ext - buf, lp, lk, 4, 1.0f + (F.absorbMark[i] == 1 ? 0.5f : 0.0f) + (dPct >= 0.15f ? 0.5f : 0.0f) });
			}
			// 4. Break-and-acceptance
			if (P.setupOn[SETUP_BREAK_ACCEPT] && i >= 2 && (reg == RG_TREND_UP || reg == RG_TREND_DOWN || reg == RG_BALANCE))
			{
				static const int hiKinds[] = { LVL_IBH, LVL_VAH, LVL_PDH, LVL_ONH, LVL_PD_VAH, LVL_LIQ_EQH, LVL_PWH, -1 };
				static const int loKinds[] = { LVL_IBL, LVL_VAL, LVL_PDL, LVL_ONL, LVL_PD_VAL, LVL_LIQ_EQL, LVL_PWL, -1 };
				const bool flowUp = imbBuy || (i >= 3 && (F.imbMark[i - 1] >= 1 || F.imbMark[i - 2] >= 1)) || F.cvdZ[i] > 0.5f;
				const bool flowDn = imbSell || (i >= 3 && (F.imbMark[i - 1] == -1 || F.imbMark[i - 2] == -1 || F.imbMark[i - 1] == 2)) || F.cvdZ[i] < -0.5f;
				for (size_t k = 0; k < lv.size(); ++k)
				{
					const float L = lv[k].price; bool hi = false, lo = false;
					for (int q = 0; hiKinds[q] >= 0; ++q) if (lv[k].kind == hiKinds[q]) hi = true;
					for (int q = 0; loKinds[q] >= 0; ++q) if (lv[k].kind == loKinds[q]) lo = true;
					if (hi && dcs >= thr && dPct > 0 && flowUp && c > L && sc.Close[i - 1] > L && sc.Close[i - 2] <= L && c - L <= 2.0f * atr)
					{ cands.push_back({ SETUP_BREAK_ACCEPT, 1, L - buf, L, lv[k].kind, 2, (imbBuy ? 1.0f : 0.5f) + (F.cvdZ[i] > 1.0f ? 0.5f : 0.0f) + (A.auctionF[i] >= 0.5f ? 0.5f : 0.0f) }); break; }
					if (lo && dcs <= -thr && dPct < 0 && flowDn && c < L && sc.Close[i - 1] < L && sc.Close[i - 2] >= L && L - c <= 2.0f * atr)
					{ cands.push_back({ SETUP_BREAK_ACCEPT, -1, L + buf, L, lv[k].kind, 2, (imbSell ? 1.0f : 0.5f) + (F.cvdZ[i] < -1.0f ? 0.5f : 0.0f) + (A.auctionF[i] <= -0.5f ? 0.5f : 0.0f) }); break; }
				}
			}
			// 5. SMT / CVD divergence reversal at liquidity
			if (P.setupOn[SETUP_DIVERGENCE] && (F.divMark[i] != 0 || I.smtMark[i] != 0))
			{
				static const int hiKinds[] = { LVL_LIQ_EQH, LVL_PDH, LVL_ONH, LVL_VAH, LVL_NAKED_POC, LVL_IBH, LVL_PD_VAH, LVL_VWAP_B2U, LVL_VWAP_B3U, LVL_PWH, -1 };
				static const int loKinds[] = { LVL_LIQ_EQL, LVL_PDL, LVL_ONL, LVL_VAL, LVL_NAKED_POC, LVL_IBL, LVL_PD_VAL, LVL_VWAP_B2D, LVL_VWAP_B3D, LVL_PWL, -1 };
				const int dir = (F.divMark[i] != 0) ? F.divMark[i] : I.smtMark[i];
				// the pivot that just got confirmed
				float pivot = dir > 0 ? FLT_MAX : -FLT_MAX;
				for (int k = static_cast<int>(A.swings.size()) - 1; k >= 0; --k) { if (A.swings[k].confirmIdx == i) { pivot = A.swings[k].price; break; } if (A.swings[k].confirmIdx < i) break; }
				float lp; int lk;
				if (dir > 0 && pivot < FLT_MAX && dcs > -thr && NearLevel(lv, pivot, tol * 1.5f, loKinds, lp, lk)) cands.push_back({ SETUP_DIVERGENCE, 1, pivot - buf, lp, lk, 3, (F.divMark[i] != 0 ? 1.0f : 0.0f) + (I.smtMark[i] != 0 ? 1.0f : 0.0f) + (F.absorbMark[i] == 1 ? 0.5f : 0.0f) });
				if (dir < 0 && pivot > -FLT_MAX && dcs < thr && NearLevel(lv, pivot, tol * 1.5f, hiKinds, lp, lk)) cands.push_back({ SETUP_DIVERGENCE, -1, pivot + buf, lp, lk, 3, (F.divMark[i] != 0 ? 1.0f : 0.0f) + (I.smtMark[i] != 0 ? 1.0f : 0.0f) + (F.absorbMark[i] == -1 ? 0.5f : 0.0f) });
			}

			// choose the strongest candidate that yields a valid trade plan
			int best = -1; int bestScore = -1;
			Signal sig;
			for (size_t k = 0; k < cands.size(); ++k)
			{
				Signal tmp;
				if (!BuildSignal(sc, S, i, cands[k], lv, tmp)) continue;
				const int score = cands[k].score * 100 + static_cast<int>(tmp.rr * 10);
				if (score > bestScore) { bestScore = score; best = static_cast<int>(k); sig = tmp; sig.grade = GradeSignal(S, i, cands[k], dcs, thr); }
			}
			// outside RTH only the configured grades are taken (thin overnight flow produces weaker setups)
			if (best >= 0 && i < static_cast<int>(S.base.isRth.size()) && !S.base.isRth[i] && (P.ethSignals == 0 || (P.ethSignals == 1 && sig.grade > 1))) best = -1;
			// live protocol: no signals outside the trade window (the open and the last half hour are left alone)
			if (best >= 0 && P.tradeEndSec > P.tradeStartSec) { const int tod = sc.BaseDateTimeIn[i].GetTimeInSeconds(); if (tod < P.tradeStartSec || tod >= P.tradeEndSec) best = -1; }
			if (best >= 0)
			{
				// no duplicate of the same setup/direction within 3 bars
				bool dup = false;
				for (int k = static_cast<int>(D.signals.size()) - 1; k >= 0 && D.signals[k].idx >= i - 3; --k) if (D.signals[k].type == sig.type && D.signals[k].dir == sig.dir) { dup = true; break; }
				if (!dup)
				{
					D.signals.push_back(sig);
					if (D.signals.size() > 5000) D.signals.erase(D.signals.begin(), D.signals.begin() + 1000);
					D.signalType[i] = static_cast<signed char>(sig.type); D.signalDir[i] = static_cast<signed char>(sig.dir);
					D.newSignals = true;
					{ char t[72]; sprintf_s(t, sizeof(t), "%s %s %c", sig.dir > 0 ? "LONG" : "SHORT", kSetupNames[sig.type], sig.grade == 1 ? 'A' : (sig.grade == 2 ? 'B' : 'C')); PushEvent(sc, S, i, EV_SIGNAL, sig.dir, sig.entry, t); }
				}
			}
		}
		D.Stamp(sc);
	}

	// ==== 12 Validation =========================================================
	// Replays each signal forward on closed bars with slippage; stop-first on ambiguous bars.
	void EnsureVal(SCStudyInterfaceRef sc, ChartState& S)
	{
		EnsureDcs(sc, S);
		const int n = sc.ArraySize; if (n <= 0) return;
		ValState& V = S.val; const ValParams& P = S.params.val; DcsState& D = S.dcs;
		Fit(V.cumR, n); Fit(V.addR, n);
		if (V.UpToDate(sc) && !D.newSignals) return;
		D.newSignals = false;
		const int lastClosed = n - 2;
		const float slip = P.slippageTicks * S.tickSize;
		int earliestRes = INT_MAX;
		for (size_t k = V.nextSignal; k < D.signals.size(); ++k)
		{
			Signal& g = D.signals[k];
			if (g.resolved != 0) continue;
			const float entryFill = g.entry + g.dir * slip;
			const float stopFill = g.stop - g.dir * slip;
			const float risk = static_cast<float>(fabs(entryFill - stopFill));
			if (risk <= 0) { g.resolved = -1; g.resultR = -1; g.resIdx = g.idx + 1; g.barsToRes = 1; }
			else
			{
				float mfe = 0, mae = 0; bool t1Hit = false; int res = 0; float resultR = 0; int resIdx = -1;
				const int last = Min(lastClosed, g.idx + Max(1, P.maxBars));
				for (int j = g.idx + 1; j <= last; ++j)
				{
					const float hi = sc.High[j], lo = sc.Low[j];
					const float fav = g.dir > 0 ? (hi - entryFill) / risk : (entryFill - lo) / risk;
					const float adv = g.dir > 0 ? (entryFill - lo) / risk : (hi - entryFill) / risk;
					const bool stopHit = g.dir > 0 ? lo <= stopFill : hi >= stopFill;
					const bool t1Now = g.dir > 0 ? hi >= g.t1 : lo <= g.t1;
					const bool t2Now = g.dir > 0 ? hi >= g.t2 : lo <= g.t2;
					if (!t1Hit) { mfe = Max(mfe, fav); mae = Max(mae, adv); }
					if (!t1Hit)
					{
						if (stopHit && (!t1Now || P.stopFirst)) { res = -1; resultR = -1.0f; resIdx = j; break; }
						if (t1Now) { t1Hit = true; resultR = static_cast<float>(fabs(g.t1 - entryFill)) / risk; resIdx = j; if (t2Now && !P.stopFirst) { g.hitT2 = 1; } }
						if (stopHit && t1Now && !P.stopFirst) { res = 1; break; }
					}
					else
					{
						// after T1: track whether T2 is reached before the stop (statistics only)
						if (stopHit) { res = 1; break; }
						if (t2Now) { g.hitT2 = 1; res = 1; break; }
					}
				}
				if (res == 0 && t1Hit) res = 1;                                   // T1 hit, T2 still open: count as a resolved win
				if (res == 0 && last == g.idx + Max(1, P.maxBars) && last <= lastClosed)
				{
					res = 2; resIdx = last; resultR = (g.dir > 0 ? sc.Close[last] - entryFill : entryFill - sc.Close[last]) / risk;   // timeout: mark to market
				}
				if (res == 0) continue;   // still pending (needs more bars); nextSignal stays at the first pending one
				g.resolved = res; g.resultR = resultR; g.resIdx = resIdx; g.barsToRes = resIdx - g.idx; g.mfeR = mfe; g.maeR = mae;
			}
			if (g.resIdx >= 0 && g.resIdx < n) { V.addR[g.resIdx] += g.resultR; earliestRes = Min(earliestRes, g.resIdx); }
			const int regAt = (g.idx < static_cast<int>(S.regime.regime.size())) ? Clamp(static_cast<int>(S.regime.regime[g.idx]), 0, 4) : 0;
			SetupStats* targets[2] = { &V.stats[Clamp(g.type, 0, SETUP_COUNT - 1)], &V.stats3[Clamp(g.type, 0, SETUP_COUNT - 1)][regAt][Clamp(g.grade, 1, 3) - 1] };
			for (int q = 0; q < 2; ++q)
			{
				SetupStats& st = *targets[q];
				++st.count; st.sumR += g.resultR; st.sumMfe += g.mfeR; st.sumMae += g.maeR; st.sumBars += g.barsToRes;
				if (g.resolved == 1) { ++st.wins; st.sumWinR += g.resultR; if (g.hitT2) ++st.t2; }
				else if (g.resolved == -1) { ++st.losses; st.sumLossR += g.resultR; }
				else { ++st.timeouts; if (g.resultR >= 0) st.sumWinR += g.resultR; else st.sumLossR += g.resultR; }
			}
			V.totalR += g.resultR;
		}
		while (V.nextSignal < static_cast<int>(D.signals.size()) && D.signals[V.nextSignal].resolved != 0) ++V.nextSignal;
		// cumulative R curve
		int from = Min(V.computedThrough + 1, earliestRes); if (from < 0) from = 0; if (from > n - 1) from = n - 1;
		if (earliestRes != INT_MAX) V.dirtyFrom = Min(V.dirtyFrom, earliestRes);
		for (int i = from; i < n; ++i) V.cumR[i] = (i > 0 ? V.cumR[i - 1] : 0.0f) + V.addR[i];
		V.Stamp(sc);
	}

	// ==== 13 Feature logger =====================================================
	namespace log_detail
	{
		inline int BarAtOrAfter(SCStudyInterfaceRef sc, int fromIdx, const SCDateTime& t, int lastClosed)
		{
			for (int k = fromIdx + 1; k <= lastClosed; ++k) if (sc.BaseDateTimeIn[k] >= t) return k;
			return -1;
		}
		inline void WriteHeader(FILE* f)
		{
			fprintf(f, "time,idx,trading_day,open,high,low,close,volume,atr");
			for (int k = 0; k < F_COUNT; ++k) fprintf(f, ",%s", kFeatureNames[k]);
			fprintf(f, ",dcs,regime,setup,dir,grade,fwd5,fwd15,fwd30,fwd60,mfe,mae\n");
		}
	}

	void EnsureLog(SCStudyInterfaceRef sc, ChartState& S)
	{
		using namespace log_detail;
		EnsureVal(sc, S);
		const int n = sc.ArraySize; if (n <= 0) return;
		LogState& L = S.log; const LogParams& P = S.params.log; const DcsState& D = S.dcs; const BaseState& B = S.base;
		if (L.UpToDate(sc)) return;
		if (!P.enabled || !S.studyPresent[E_LOG]) { L.Stamp(sc); return; }
		const int lastClosed = n - 2;
		// queue new closed bars
		for (int i = Max(0, L.lastQueuedIdx + 1); i <= lastClosed; ++i)
		{
			if (P.rthOnly && !B.isRth[i]) { L.lastQueuedIdx = i; continue; }
			if (!P.rewriteOnRecalc && sc.BaseDateTimeIn[i].GetAsDouble() <= L.lastWrittenTime) { L.lastQueuedIdx = i; continue; }
			LogRow r; r.idx = i; r.t = sc.BaseDateTimeIn[i].GetAsDouble(); r.o = sc.Open[i]; r.h = sc.High[i]; r.l = sc.Low[i]; r.c = sc.Close[i]; r.v = sc.Volume[i];
			for (int k = 0; k < F_COUNT; ++k) r.feat[k] = D.feat[static_cast<size_t>(i) * F_COUNT + k];
			r.dcs = D.dcs[i]; r.regime = S.regime.regime[i]; r.setup = D.signalType[i]; r.dir = D.signalDir[i]; r.atr = AtrAt(S, i);
			r.grade = 0; if (r.setup != 0) for (int k = static_cast<int>(D.signals.size()) - 1; k >= 0 && D.signals[k].idx >= i; --k) if (D.signals[k].idx == i) { r.grade = D.signals[k].grade; break; }
			r.fwd5 = r.fwd15 = r.fwd30 = r.fwd60 = r.mfe = r.mae = 0; r.done = false; r.tradingDay = B.tradingDay[i];
			L.pending.push_back(r);
			L.lastQueuedIdx = i;
		}
		// complete rows whose 60-minute horizon has closed, then write them in order
		size_t ready = 0;
		for (size_t q = 0; q < L.pending.size(); ++q)
		{
			LogRow& r = L.pending[q];
			if (!r.done)
			{
				const SCDateTime t0 = sc.BaseDateTimeIn[r.idx];
				SCDateTime t60 = t0; t60.AddMinutes(60);
				const int k60 = BarAtOrAfter(sc, r.idx, t60, lastClosed);
				if (k60 < 0) break;
				SCDateTime t5 = t0; t5.AddMinutes(5); SCDateTime t15 = t0; t15.AddMinutes(15); SCDateTime t30 = t0; t30.AddMinutes(30);
				const int k5 = BarAtOrAfter(sc, r.idx, t5, lastClosed), k15 = BarAtOrAfter(sc, r.idx, t15, lastClosed), k30 = BarAtOrAfter(sc, r.idx, t30, lastClosed);
				const float a = r.atr > 0 ? r.atr : S.tickSize;
				r.fwd5 = k5 >= 0 ? (sc.Close[k5] - r.c) / a : 0; r.fwd15 = k15 >= 0 ? (sc.Close[k15] - r.c) / a : 0; r.fwd30 = k30 >= 0 ? (sc.Close[k30] - r.c) / a : 0; r.fwd60 = (sc.Close[k60] - r.c) / a;
				float hi = -FLT_MAX, lo = FLT_MAX;
				for (int k = r.idx + 1; k <= k60; ++k) { hi = Max(hi, sc.High[k]); lo = Min(lo, sc.Low[k]); }
				r.mfe = (hi - r.c) / a; r.mae = (r.c - lo) / a;
				r.done = true;
			}
			ready = q + 1;
		}
		if (ready > 0)
		{
			if (L.path.empty())
			{
				SCString path = sc.DataFilesFolder();
				if (path.GetLength() > 0 && path.GetChars()[path.GetLength() - 1] != '\\' && path.GetChars()[path.GetLength() - 1] != '/') path += "\\";
				std::string sym = sc.Symbol.GetChars();
				for (size_t k = 0; k < sym.size(); ++k) if (sym[k] == '\\' || sym[k] == '/' || sym[k] == ':' || sym[k] == '*' || sym[k] == '?' || sym[k] == '"' || sym[k] == '<' || sym[k] == '>' || sym[k] == '|') sym[k] = '_';
				L.path = std::string(path.GetChars()) + P.prefix + "_" + sym + ".csv";
			}
			FILE* f = nullptr;
			if (!L.headerWritten)
			{
				const bool rewrite = P.rewriteOnRecalc != 0;
				f = fopen(L.path.c_str(), rewrite ? "w" : "a");
				if (f)
				{
					if (rewrite) WriteHeader(f);
					else { fseek(f, 0, SEEK_END); if (ftell(f) == 0) WriteHeader(f); }
					L.headerWritten = true; L.generationWritten = L.generation;
				}
			}
			else f = fopen(L.path.c_str(), "a");
			if (f)
			{
				for (size_t q = 0; q < ready; ++q)
				{
					const LogRow& r = L.pending[q];
					SCDateTime dt(r.t); int Y, M, Dd, hh, mm, ss; dt.GetDateTimeYMDHMS(Y, M, Dd, hh, mm, ss);
					fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d,%d,%d,%.6g,%.6g,%.6g,%.6g,%.6g,%.6g", Y, M, Dd, hh, mm, ss, r.idx, r.tradingDay, r.o, r.h, r.l, r.c, r.v, r.atr);
					for (int k = 0; k < F_COUNT; ++k) { if (IsNan(r.feat[k])) fputs(",", f); else fprintf(f, ",%.4f", r.feat[k]); }
					fprintf(f, ",%.2f,%d,%d,%d,%d,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f\n", r.dcs, r.regime, r.setup, r.dir, r.grade, r.fwd5, r.fwd15, r.fwd30, r.fwd60, r.mfe, r.mae);
				}
				fclose(f);
				L.rowsWritten += static_cast<int>(ready);
				L.lastWrittenTime = Max(L.lastWrittenTime, L.pending[ready - 1].t);
			}
			L.pending.erase(L.pending.begin(), L.pending.begin() + ready);
		}
		L.Stamp(sc);
	}

	// ==== 14 HUD snapshot, warnings, renderer ===================================

	void CheckWarnings(SCStudyInterfaceRef sc, ChartState& S)
	{
		Warnings& W = S.warn;
		W.storageNotTick = (sc.IntradayDataStorageTimeUnit != kStorageUnitTick);
		W.tzNotNY = false;
		{
			// offset between the chart's clock and UTC: New York is -5 (EST) or -4 (EDT)
			const SCDateTime nowChart = sc.CurrentSystemDateTime;
			const SCDateTime nowUtc = sc.AdjustDateTimeToGMT(nowChart);
			const double offH = (nowChart.GetAsDouble() - nowUtc.GetAsDouble()) * 24.0;
			const bool ny = fabs(offH + 5.0) < 0.1 || fabs(offH + 4.0) < 0.1;
			W.tzNotNY = !ny && fabs(offH) > 0.01;   // an unknown (zero) offset is not reported
		}
		if (false && sc.GetChartTimeZone != nullptr)
		{
			SCString tz = sc.GetChartTimeZone(sc.ChartNumber);
			const char* s = tz.GetChars();
			std::string t = s ? s : "";
			for (size_t k = 0; k < t.size(); ++k) t[k] = static_cast<char>(tolower(static_cast<unsigned char>(t[k])));
			W.tzNotNY = !(t.find("new_york") != std::string::npos || t.find("new york") != std::string::npos || t.find("eastern") != std::string::npos
				|| t.compare(0, 3, "est") == 0 || t.find("est-05") != std::string::npos || t.find("est-5") != std::string::npos);
			if (t.empty()) W.tzNotNY = false;   // unknown: do not warn
		}
		W.vapOff = (sc.MaintainVolumeAtPriceData == 0);
		W.noDepth = (sc.GetBidMarketDepthNumberOfLevels != nullptr) ? (sc.GetBidMarketDepthNumberOfLevels() <= 0) : true;
		W.text[0] = 0;
		if (W.storageNotTick) strcat_s(W.text, sizeof(W.text), "Storage unit must be 1 TICK (Global Settings>>Data/Trade Service Settings)\n");
		if (W.tzNotNY) strcat_s(W.text, sizeof(W.text), "Chart time zone must be New York (Chart>>Chart Settings)\n");
		if (W.vapOff) strcat_s(W.text, sizeof(W.text), "Volume at Price data off (reload chart after adding NQ Edge studies)\n");
		if (W.interMissing[0]) { strcat_s(W.text, sizeof(W.text), "Missing intermarket: "); strcat_s(W.text, sizeof(W.text), W.interMissing); strcat_s(W.text, sizeof(W.text), "\n"); }
		static const char* engineStudy[E_COUNT] = { "", "Auction/Structure Engine", "VWAP Engine", "Order Flow Engine", "Regime + MTF Bias", "Intermarket Engine", "Directional Conviction Score", "Signal Validation", "Feature Logger" };
		for (int e = E_AUCTION; e <= E_DCS; ++e)
			if (!S.studyPresent[e] && S.hudUpdates > 1 && !S.terminalPresent) { strcat_s(W.text, sizeof(W.text), "Add study: NQ Edge: "); strcat_s(W.text, sizeof(W.text), engineStudy[e]); strcat_s(W.text, sizeof(W.text), "\n"); }
		if (W.noDepth) strcat_s(W.text, sizeof(W.text), "No market depth (optional; depth features disabled)\n");
	}

	void BuildHudSnapshot(SCStudyInterfaceRef sc, ChartState& S)
	{
		using namespace dcs_detail;
		HudSnapshot& H = S.hud;
		const int n = sc.ArraySize;
		const int i = n - 2;   // last closed bar
		H.lastClosedIdx = i;
		if (i < 0) return;
		H.close = sc.Close[n - 1];
		H.atr = AtrAt(S, i);
		const float thr = S.params.dcs.signalThr > 0 ? S.params.dcs.signalThr : S.dcs.weights.thrSignal;
		if (i < static_cast<int>(S.dcs.dcs.size()))
		{
			H.dcs = S.dcs.dcs[i]; H.dcsSmooth = S.dcs.dcsSmooth[i];
			H.dcsTrend = (i >= 5) ? S.dcs.dcs[i] - S.dcs.dcs[i - 5] : 0;
			H.barState = S.dcs.barState[i];
			H.bias = (H.dcs >= thr) ? 1 : (H.dcs <= -thr ? -1 : 0);
		}
		if (i < static_cast<int>(S.regime.regime.size()))
		{
			H.regime = S.regime.regime[i];
			H.mtf[0] = S.regime.mtf1[i]; H.mtf[1] = S.regime.mtf5[i]; H.mtf[2] = S.regime.mtf15[i]; H.mtf[3] = S.regime.mtf60[i];
			for (int t = 0; t < 4; ++t) H.mtfAvail[t] = S.regime.tf[t].available;
		}
		if (i < static_cast<int>(S.auction.openType.size())) { H.openType = S.auction.openType[i]; H.valueMig = S.auction.valueMig[i]; }
		if (i < static_cast<int>(S.flow.cvdZ.size()))
		{
			H.cvdZ = S.flow.cvdZ[i]; H.cvdTrend = H.cvdZ > 0.5f ? 1 : (H.cvdZ < -0.5f ? -1 : 0);
			strcpy_s(H.lastEvent, sizeof(H.lastEvent), S.flow.lastEventText); H.lastEventPrice = S.flow.lastEventPrice;
		}
		// intermarket
		{
			const InterState& I = S.inter;
			H.ymAvail = I.ym.ok; H.tickAvail = I.tick.ok;
			H.ym = (I.ym.ok && i < static_cast<int>(I.rsYM.size())) ? (I.rsYM[i] > 0.2f ? 1 : (I.rsYM[i] < -0.2f ? -1 : 0)) : 0;
			H.esAvail = I.es.ok; H.rtyAvail = I.rty.ok;
			H.es = (I.es.ok && i < static_cast<int>(I.rsES.size())) ? (I.rsES[i] > 0.2f ? 1 : (I.rsES[i] < -0.2f ? -1 : 0)) : 0;
			H.rty = (I.rty.ok && i < static_cast<int>(I.rsRTY.size())) ? (I.rsRTY[i] > 0.2f ? 1 : (I.rsRTY[i] < -0.2f ? -1 : 0)) : 0;
			H.tickVal = (I.tick.ok && i < static_cast<int>(I.tickCum.size()) && !IsNan(I.tickCum[i])) ? I.tickCum[i] : 0.0f;
			H.tick = (I.tick.ok && i < static_cast<int>(I.tickCum.size()) && !IsNan(I.tickCum[i])) ? (I.tickCum[i] > 0.15f ? 1 : (I.tickCum[i] < -0.15f ? -1 : 0)) : 0;
			H.megaCount = 0;
			for (int k = 0; k < 6; ++k)
			{
				if (!I.mega[k].ok) continue;
				H.mega[H.megaCount] = I.megaState[k];
				const char* sym = I.mega[k].symbol.GetChars();
				char nm[12] = ""; int q = 0;
				for (const char* p = sym ? sym : ""; *p && q < 6 && *p != '-' && *p != '.'; ++p) nm[q++] = *p;
				nm[q] = 0; if (q == 0) sprintf_s(nm, sizeof(nm), "M%d", k + 1);
				strcpy_s(H.megaNames[H.megaCount], sizeof(H.megaNames[H.megaCount]), nm);
				++H.megaCount;
			}
			H.smt = (I.lastSmtIdx >= 0 && i - I.lastSmtIdx <= 12) ? (I.lastSmtVal > 0 ? 1 : -1) : 0;
			const InterParams& IP = S.params.inter;
			H.interConfigured = (IP.chartYM > 0) + (IP.chartES > 0) + (IP.chartRTY > 0) + (IP.chartTICK > 0);
			for (int k = 0; k < 6; ++k) H.interConfigured += (IP.chartMega[k] > 0);
			H.interConnected = 0; for (int b = 0; b < 10; ++b) H.interConnected += (I.available >> b) & 1;
			strncpy_s(H.leadLag, sizeof(H.leadLag), I.leadText, _TRUNCATE);
		}
		H.lastEventIdx = S.flow.lastEventIdx;
		H.legR2 = static_cast<float>(S.flow.legReg.r2); H.legDir = S.flow.legReg.slope > 0 ? 1 : (S.flow.legReg.slope < 0 ? -1 : 0);
		GroupAgreement(S.dcs, i, H.dcs >= 0 ? 1 : -1, H.agree, H.agreeN);
		// day type guess + range vs ADR
		{
			const AuctionState& A = S.auction;
			double adr = 0; int m = 0;
			for (int k = static_cast<int>(A.rthRanges.size()) - 1; k >= 0 && m < Max(1, S.params.auction.adrDays); --k, ++m) adr += A.rthRanges[k];
			adr = m > 0 ? adr / m : 0;
			float rng = (A.rthHigh > -FLT_MAX && A.rthLow < FLT_MAX) ? A.rthHigh - A.rthLow : 0.0f;
			H.adrPrev = false; if (rng <= 0 && !A.rthRanges.empty()) { rng = A.rthRanges.back(); H.adrPrev = true; }
			H.adrPct = adr > 0 ? static_cast<float>(100.0 * rng / adr) : 0.0f;
			const float ib = (A.ibClosed && A.ibH > A.ibL) ? A.ibH - A.ibL : 0.0f;
			if (i < static_cast<int>(S.base.isRth.size()) && !S.base.isRth[i]) strcpy_s(H.dayType, sizeof(H.dayType), "ETH");
			else if (ib <= 0) strcpy_s(H.dayType, sizeof(H.dayType), "IB forming");
			else if (rng >= 1.8f * ib && (H.regime == RG_TREND_UP || H.regime == RG_TREND_DOWN)) strcpy_s(H.dayType, sizeof(H.dayType), "Trend day");
			else if (rng >= 1.3f * ib) strcpy_s(H.dayType, sizeof(H.dayType), "Normal var.");
			else if (rng <= 1.1f * ib) strcpy_s(H.dayType, sizeof(H.dayType), "Balance day");
			else strcpy_s(H.dayType, sizeof(H.dayType), "Normal day");
		}
		H.signalsTotal = static_cast<int>(S.dcs.signals.size());
		if (i < static_cast<int>(S.vwap.vwap.size()) && S.vwap.vwap[i] > 0)
			sprintf_s(H.vwapText, sizeof(H.vwapText), "VWAP %s", sc.FormatGraphValue(S.vwap.vwap[i], sc.BaseGraphValueFormat).GetChars());
		else H.vwapText[0] = 0;

		// nearest support / resistance from the level list as of the last closed bar
		std::vector<Lv> lv; GatherLevels(sc, S, i, lv);
		H.supPrice = 0; H.resPrice = 0; H.supKind = H.resKind = LVL_NONE;
		float bs = FLT_MAX, br = FLT_MAX;
		for (size_t k = 0; k < lv.size(); ++k)
		{
			const int kd = lv[k].kind;
			if (kd == LVL_VWAP_B1U || kd == LVL_VWAP_B1D || kd == LVL_VWAP_B2U || kd == LVL_VWAP_B2D || kd == LVL_VWAP_B3U || kd == LVL_VWAP_B3D || kd == LVL_ABSORB || kd == LVL_IMB || kd == LVL_FAILED) continue;
			const float d = lv[k].price - H.close;
			if (d > 0 && d < br) { br = d; H.resPrice = lv[k].price; H.resKind = lv[k].kind; }
			if (d < 0 && -d < bs) { bs = -d; H.supPrice = lv[k].price; H.supKind = lv[k].kind; }
		}

		// current setup type = the most recent signal's type (for the stats row) or the one the state line suggests
		H.curSetup = SETUP_NONE; H.statsAvail = false;
		if (!S.dcs.signals.empty()) { H.curSetup = S.dcs.signals.back().type; }
		else H.curSetup = (H.regime == RG_TREND_UP || H.regime == RG_TREND_DOWN) ? SETUP_TREND_PULLBACK : SETUP_VALUE_EDGE;
		if (H.curSetup > 0 && H.curSetup < SETUP_COUNT) { H.curStats = S.val.stats[H.curSetup]; H.statsAvail = true; }

		// plain-English state line
		const char* vwapS = S.vwap.vwap.size() > static_cast<size_t>(i) && S.vwap.vwap[i] > 0 ? sc.FormatGraphValue(S.vwap.vwap[i], sc.BaseGraphValueFormat).GetChars() : "VWAP";
		// the live signal: the newest one while the validation engine has not resolved it (stop / T1 / timeout)
		const Signal* live = (!S.dcs.signals.empty() && S.dcs.signals.back().resolved == 0 && i - S.dcs.signals.back().idx <= S.params.val.maxBars) ? &S.dcs.signals.back() : nullptr;
		H.liveSize = live ? live->size : 0;
		if (live)
		{
			sprintf_s(H.stateLine, sizeof(H.stateLine), "%s %s %c: entry %s", live->dir > 0 ? "LONG" : "SHORT", kSetupNames[live->type], live->grade == 1 ? 'A' : (live->grade == 2 ? 'B' : 'C'), sc.FormatGraphValue(live->entry, sc.BaseGraphValueFormat).GetChars());
			char sz[24] = ""; if (live->size > 0) sprintf_s(sz, sizeof(sz), " | size %dx", live->size);
			sprintf_s(H.stateLine2, sizeof(H.stateLine2), "stop %s | T1 %s | R:R %.1f%s", sc.FormatGraphValue(live->stop, sc.BaseGraphValueFormat).GetChars(), sc.FormatGraphValue(live->t1, sc.BaseGraphValueFormat).GetChars(), live->rr, sz);
		}
		else if (H.regime == RG_TREND_UP)
		{
			sprintf_s(H.stateLine, sizeof(H.stateLine), "Buy pullback to %s %s", H.supKind != LVL_NONE ? LevelName(H.supKind) : "VWAP", H.supPrice > 0 ? sc.FormatGraphValue(H.supPrice, sc.BaseGraphValueFormat).GetChars() : vwapS);
			sprintf_s(H.stateLine2, sizeof(H.stateLine2), "trigger absorb/delta flip | invalid < %s", H.supPrice > 0 ? sc.FormatGraphValue(H.supPrice - H.atr, sc.BaseGraphValueFormat).GetChars() : "?");
		}
		else if (H.regime == RG_TREND_DOWN)
		{
			sprintf_s(H.stateLine, sizeof(H.stateLine), "Sell pullback to %s %s", H.resKind != LVL_NONE ? LevelName(H.resKind) : "VWAP", H.resPrice > 0 ? sc.FormatGraphValue(H.resPrice, sc.BaseGraphValueFormat).GetChars() : vwapS);
			sprintf_s(H.stateLine2, sizeof(H.stateLine2), "trigger absorb/delta flip | invalid > %s", H.resPrice > 0 ? sc.FormatGraphValue(H.resPrice + H.atr, sc.BaseGraphValueFormat).GetChars() : "?");
		}
		else if (H.regime == RG_BALANCE)
		{
			sprintf_s(H.stateLine, sizeof(H.stateLine), "Fade %s %s / %s %s", H.resKind != LVL_NONE ? LevelName(H.resKind) : "VAH", H.resPrice > 0 ? sc.FormatGraphValue(H.resPrice, sc.BaseGraphValueFormat).GetChars() : "", H.supKind != LVL_NONE ? LevelName(H.supKind) : "VAL", H.supPrice > 0 ? sc.FormatGraphValue(H.supPrice, sc.BaseGraphValueFormat).GetChars() : "");
			strcpy_s(H.stateLine2, sizeof(H.stateLine2), "trigger absorb/exhaust | tgt POC | inv. acceptance");
		}
		else if (H.regime == RG_CHOP)
		{
			strcpy_s(H.stateLine, sizeof(H.stateLine), "Volatile chop: stand aside");
			sprintf_s(H.stateLine2, sizeof(H.stateLine2), "need DCS beyond %+.0f and a regime change", thr);
		}
		else
		{
			strcpy_s(H.stateLine, sizeof(H.stateLine), "Warming up");
			strcpy_s(H.stateLine2, sizeof(H.stateLine2), "need swings, value area and session data");
		}
		// action line: the one thing to do right now
		#define HPX(v) sc.FormatGraphValue((v), sc.BaseGraphValueFormat).GetChars()
		H.actionKind = 0; H.liveR = 0;
		if (live)
		{
			const float risk = static_cast<float>(fabs(live->entry - live->stop));
			H.liveR = risk > 0 ? (live->dir > 0 ? (H.close - live->entry) : (live->entry - H.close)) / risk : 0.0f;
			char sz[16] = ""; if (live->size > 0) sprintf_s(sz, sizeof(sz), " | %dx", live->size);
			if (i == live->idx)
			{
				H.actionKind = live->dir;
				sprintf_s(H.action, sizeof(H.action), "%s NOW %s | stop %s | T1 %s%s", live->dir > 0 ? "BUY" : "SELL", HPX(live->entry), HPX(live->stop), HPX(live->t1), sz);
			}
			else
			{
				H.actionKind = live->dir > 0 ? 2 : -2;
				sprintf_s(H.action, sizeof(H.action), "IN %s %+.1fR | stop %s | T1 %s%s", live->dir > 0 ? "LONG" : "SHORT", H.liveR, HPX(live->stop), HPX(live->t1), H.liveR >= 1.0f ? " | trail to entry" : "");
			}
		}
		else if (H.regime == RG_CHOP) { H.actionKind = 3; strcpy_s(H.action, sizeof(H.action), "NO TRADE | volatile chop"); }
		else if (H.regime == RG_NONE) { H.actionKind = 3; strcpy_s(H.action, sizeof(H.action), "NO TRADE | warming up"); }
		else sprintf_s(H.action, sizeof(H.action), "WAIT | %s", H.stateLine);
		#undef HPX
	}

	namespace render
	{
		inline uint32_t Blend(uint32_t a, uint32_t b, float t)
		{
			t = Clamp(t, 0.0f, 1.0f);
			return RGB(static_cast<int>(GetRValue(a) * (1 - t) + GetRValue(b) * t + 0.5f), static_cast<int>(GetGValue(a) * (1 - t) + GetGValue(b) * t + 0.5f), static_cast<int>(GetBValue(a) * (1 - t) + GetBValue(b) * t + 0.5f));
		}
		inline n_ACSIL::s_GraphicsColor GC(uint32_t c) { n_ACSIL::s_GraphicsColor g; g.SetColorValue(c); return g; }

		// Formats 1234 -> "1.2k", 1500000 -> "1.5M"
		inline void Abbrev(double v, char* out, size_t n)
		{
			const double a = fabs(v);
			if (a >= 1e6) sprintf_s(out, n, "%.1fM", v / 1e6);
			else if (a >= 1e4) sprintf_s(out, n, "%.0fk", v / 1e3);
			else if (a >= 1e3) sprintf_s(out, n, "%.1fk", v / 1e3);
			else sprintf_s(out, n, "%.0f", v);
		}

		struct Frame
		{
			SCStudyInterfaceRef sc; ChartState& S; const VisualConfig& V; const Theme& T;
			int n, lastClosed, firstVis, lastVis, left, top, right, bottom, region;
			int xLast, spacing, fillLeft, fillRight, fillW;
			bool fillVisible, hasTransparent;
			float tick, atr;
			int curFontPt; bool curBold; int fontH;
			Frame(SCStudyInterfaceRef s, ChartState& st, int reg)
				: sc(s), S(st), V(st.vis), T(st.vis.theme), n(s.ArraySize), lastClosed(s.ArraySize - 2), firstVis(Max(0, s.IndexOfFirstVisibleBar)), lastVis(Min(s.ArraySize - 1, s.IndexOfLastVisibleBar)),
				left(s.StudyRegionLeftCoordinate), top(s.StudyRegionTopCoordinate), right(s.StudyRegionRightCoordinate), bottom(s.StudyRegionBottomCoordinate), region(reg),
				xLast(0), spacing(0), fillLeft(0), fillRight(0), fillW(0), fillVisible(false), hasTransparent(false), tick(st.tickSize), atr(0), curFontPt(-1), curBold(false), fontH(12)
			{
				if (n <= 0) return;
				hasTransparent = sc.Graphics.FillRectangleWithColorTransparent != nullptr;
				xLast = sc.BarIndexToXPixelCoordinate(lastVis);
				const int xPrev = lastVis > 0 ? sc.BarIndexToXPixelCoordinate(lastVis - 1) : xLast - 8;
				spacing = Max(1, xLast - xPrev);
				if (spacing <= 1 && sc.ChartBarSpacing > 0) spacing = sc.ChartBarSpacing;
				fillVisible = (lastVis >= n - 1);
				fillLeft = fillVisible ? xLast + spacing : right;
				fillRight = right - 2;
				fillW = Max(0, fillRight - fillLeft);
				atr = AtrAt(S, lastClosed);
				Font(V.fontPt, false);
			}
			int XOf(int idx) const
			{
				if (idx <= lastVis) return sc.BarIndexToXPixelCoordinate(idx);
				return xLast + (idx - lastVis) * spacing;
			}
			int YOf(float price) const { return sc.RegionValueToYPixelCoordinate(price, region); }
			int YOfRegion(float value, int reg) const { return sc.RegionValueToYPixelCoordinate(value, reg); }
			void Clip(int l, int t, int r, int b) const { if (sc.Graphics.SetClippingRegionFromRectangle) { n_ACSIL::s_GraphicsRectangle rc; rc.Left = l; rc.Top = t; rc.Right = r; rc.Bottom = b; sc.Graphics.SetClippingRegionFromRectangle(rc); } }
			void Unclip() const { if (sc.Graphics.ResetClippingRegion) sc.Graphics.ResetClippingRegion(); }

			// alpha = opacity percent (100 = opaque)
			void Fill(int l, int t, int r, int b, uint32_t c, int alpha = 100) const
			{
				if (r <= l || b <= t) return;
				n_ACSIL::s_GraphicsRectangle rc; rc.Left = l; rc.Top = t; rc.Right = r; rc.Bottom = b;
				if (alpha >= 100 || !hasTransparent) { if (alpha < 100) c = Blend(T.bg, c, alpha / 100.0f); sc.Graphics.FillRectangleWithColor(rc, GC(c)); }
				else sc.Graphics.FillRectangleWithColorTransparent(rc, GC(c), static_cast<uint8_t>(100 - Clamp(alpha, 0, 100)));
			}
			void Pen(uint32_t c, int w = 1, int style = 0) const
			{
				n_ACSIL::s_GraphicsPen p; p.m_PenColor.SetColorValue(c); p.m_Width = Max(1, w);
				p.m_PenStyle = style == 1 ? n_ACSIL::s_GraphicsPen::e_PenStyle::PEN_STYLE_DASH : (style == 2 ? n_ACSIL::s_GraphicsPen::e_PenStyle::PEN_STYLE_DOT : n_ACSIL::s_GraphicsPen::e_PenStyle::PEN_STYLE_SOLID);
				sc.Graphics.SetPen(p);
			}
			void Line(int x1, int y1, int x2, int y2, uint32_t c, int w = 1, int style = 0) const { Pen(c, w, style); sc.Graphics.MoveTo(x1, y1); sc.Graphics.LineTo(x2, y2); }
			void Box(int l, int t, int r, int b, uint32_t c, int w = 1, int style = 0) const
			{
				Pen(c, w, style);
				sc.Graphics.MoveTo(l, t); sc.Graphics.LineTo(r, t); sc.Graphics.LineTo(r, b); sc.Graphics.LineTo(l, b); sc.Graphics.LineTo(l, t);
			}
			void Circle(int cx, int cy, int r, uint32_t fill, uint32_t outline) const
			{
				n_ACSIL::s_GraphicsBrush b; b.m_BrushType = n_ACSIL::s_GraphicsBrush::BRUSH_TYPE_SOLID; b.m_BrushColor.SetColorValue(fill);
				Pen(outline, 1);
				sc.Graphics.FillEllipse(cx - r, cy - r, cx + r, cy + r, b);
			}
			void Font(int pt, bool bold)
			{
				if (pt == curFontPt && bold == curBold) return;
				n_ACSIL::s_GraphicsFont f; f.m_FaceName = "Consolas"; f.m_Height = pt; f.m_Weight = bold ? FW_BOLD : FW_NORMAL;
				sc.Graphics.SetTextFont(f);
				curFontPt = pt; curBold = bold;
				n_ACSIL::s_GraphicsSize sz; sc.Graphics.GetTextSize(SCString("Hg"), sz); fontH = Max(8, sz.Height);
			}
			int TextW(const char* s) const { n_ACSIL::s_GraphicsSize sz; sc.Graphics.GetTextSize(SCString(s), sz); return sz.Width; }
			void Text(int x, int y, const char* s, uint32_t c) const { sc.Graphics.SetTextColor(GC(c)); sc.Graphics.DrawTextAt(SCString(s), x, y); }
			void TextRight(int xr, int y, const char* s, uint32_t c) const { Text(xr - TextW(s), y, s, c); }
			void TextCenter(int xc, int y, const char* s, uint32_t c) const { Text(xc - TextW(s) / 2, y, s, c); }
			// Compact pill: filled rounded-ish box with text; returns its width
			int Pill(int x, int y, const char* s, uint32_t bg, uint32_t fg, int alpha = 100, bool rightAlign = false)
			{
				const int w = TextW(s) + 8, h = fontH + 2;
				const int l = rightAlign ? x - w : x;
				Fill(l, y, l + w, y + h, bg, alpha);
				Text(l + 4, y + 1, s, fg);
				return w;
			}
			void Arrow(int x1, int y1, int x2, int y2, uint32_t c, int w, int style) const
			{
				Line(x1, y1, x2, y2, c, w, style);
				const double dx = x2 - x1, dy = y2 - y1, len = sqrt(dx * dx + dy * dy); if (len < 1) return;
				const double ux = dx / len, uy = dy / len, sz = 6;
				n_ACSIL::s_GraphicsPoint pts[3];
				pts[0].X = x2; pts[0].Y = y2;
				pts[1].X = static_cast<int>(x2 - ux * sz - uy * sz * 0.6); pts[1].Y = static_cast<int>(y2 - uy * sz + ux * sz * 0.6);
				pts[2].X = static_cast<int>(x2 - ux * sz + uy * sz * 0.6); pts[2].Y = static_cast<int>(y2 - uy * sz - ux * sz * 0.6);
				n_ACSIL::s_GraphicsBrush b; b.m_BrushType = n_ACSIL::s_GraphicsBrush::BRUSH_TYPE_SOLID; b.m_BrushColor.SetColorValue(c); sc.Graphics.SetBrush(b);
				Pen(c, 1); sc.Graphics.DrawPolygon(pts);
			}
			bool Visible(int idx) const { return idx >= firstVis && idx <= lastVis; }
			const char* Px(float price) const { return sc.FormatGraphValue(price, sc.BaseGraphValueFormat).GetChars(); }
		};

		inline uint32_t DcsColor(const Theme& T, float dcs)
		{
			const float t = Clamp(static_cast<float>(fabs(dcs)) / 100.0f, 0.0f, 1.0f);
			return dcs >= 0 ? Blend(T.neutral, T.bull, t) : Blend(T.neutral, T.bear, t);
		}
		inline uint32_t StateColor(const Theme& T, int state)
		{
			switch (state) { case 2: return T.bull; case 1: return Blend(T.neutral, T.bull, 0.55f); case -1: return Blend(T.neutral, T.bear, 0.55f); case -2: return T.bear; default: return T.neutral; }
		}

	} // namespace render

	namespace render
	{
		// ---------------- Footprint: bid x ask cells with delta/volume heat, imbalances, POC, unfinished auctions ----------------
		// Bar spacing >= 36 px: "bid x ask" in every cell plus delta above / volume below; >= 20 px: the level delta; narrower:
		// heat only. Under 12 px the layer draws nothing (the bias candles remain). A 1 px frame in the bar's conviction
		// colour keeps the bias readable under the cells.
		void DrawFootprint(Frame& F)
		{
			SCStudyInterfaceRef sc = F.sc; ChartState& S = F.S; const Theme& T = F.T; const VisualConfig& V = F.V; const FlowParams& P = S.params.flow;
			if (sc.VolumeAtPriceForBars == nullptr || F.spacing < 12) return;
			const int n = sc.ArraySize;
			const int cellW = Max(6, F.spacing - 2), half = cellW / 2;
			const int mode = cellW >= 34 ? 2 : (cellW >= 18 ? 1 : 0);
			F.Font(Max(7, V.fontPt - 2), false);
			const int fh = F.fontH;
			const double ratio = P.imbRatioPct / 100.0, minV = P.imbMinVolume;
			const float weak = S.params.dcs.weakThr;
			char buf[32];
			std::vector<const s_VolumeAtPriceV2*> lv; lv.reserve(64);
			std::vector<signed char> imb; std::vector<unsigned char> stacked;
			const int nVap = static_cast<int>(sc.VolumeAtPriceForBars->GetNumberOfBars());
			for (int i = Max(0, F.firstVis); i <= F.lastVis && i < nVap && i < n; ++i)
			{
				const int cnt = sc.VolumeAtPriceForBars->GetSizeAtBarIndex(i);
				if (cnt <= 0) continue;
				lv.clear();
				for (int q = 0; q < cnt; ++q) { const s_VolumeAtPriceV2* pp = nullptr; if (sc.VolumeAtPriceForBars->GetVAPElementAtIndex(i, q, &pp) && pp) lv.push_back(pp); }
				const int m = static_cast<int>(lv.size()); if (m == 0) continue;
				double maxAbs = 1, maxVol = 1; int pocQ = 0;
				for (int q = 0; q < m; ++q)
				{
					maxAbs = Max(maxAbs, fabs(static_cast<double>(lv[q]->AskVolume) - static_cast<double>(lv[q]->BidVolume)));
					if (static_cast<double>(lv[q]->Volume) > maxVol) { maxVol = static_cast<double>(lv[q]->Volume); pocQ = q; }
				}
				const bool forming = (i == n - 1);
				const int x = F.XOf(i);
				imb.assign(static_cast<size_t>(m), 0); stacked.assign(static_cast<size_t>(m), 0);
				for (int q = 0; q < m; ++q)
				{
					const bool adjBelow = q > 0 && lv[q]->PriceInTicks - lv[q - 1]->PriceInTicks == 1;
					const bool adjAbove = q + 1 < m && lv[q + 1]->PriceInTicks - lv[q]->PriceInTicks == 1;
					if (adjBelow && lv[q]->AskVolume >= minV && lv[q - 1]->BidVolume >= minV && lv[q]->AskVolume >= ratio * lv[q - 1]->BidVolume) imb[q] = 1;
					if (adjAbove && lv[q]->BidVolume >= minV && lv[q + 1]->AskVolume >= minV && lv[q]->BidVolume >= ratio * lv[q + 1]->AskVolume) imb[q] = static_cast<signed char>(imb[q] == 1 ? 2 : -1);
				}
				for (int side = -1; side <= 1; side += 2)
				{
					int run = 0;
					for (int q = 0; q <= m; ++q)
					{
						const bool onq = q < m && (imb[q] == side || imb[q] == 2) && (run == 0 || (q > 0 && lv[q]->PriceInTicks - lv[q - 1]->PriceInTicks == 1));
						if (onq) ++run; else { if (run >= P.imbStackLevels) for (int r = q - run; r < q; ++r) stacked[r] = 1; run = 0; }
					}
				}
				int yTop = INT_MAX, yBot = INT_MIN;
				for (int q = 0; q < m; ++q)
				{
					const float price = lv[q]->PriceInTicks * F.tick;
					int y1 = F.YOf(price + 0.5f * F.tick), y2 = F.YOf(price - 0.5f * F.tick);
					if (y2 < F.top || y1 > F.bottom) continue;
					if (y2 - y1 < 2) y2 = y1 + 2;
					yTop = Min(yTop, y1); yBot = Max(yBot, y2);
					const double d = static_cast<double>(lv[q]->AskVolume) - static_cast<double>(lv[q]->BidVolume);
					const float inten = static_cast<float>(0.10 + 0.45 * fabs(d) / maxAbs + 0.30 * static_cast<double>(lv[q]->Volume) / maxVol);
					uint32_t c = Blend(T.bg, d >= 0 ? T.bull : T.bear, Min(0.85f, inten));
					if (forming) c = Blend(c, T.bg, 0.35f);
					F.Fill(x - half, y1, x + half, y2, c, 100);
					if ((y2 - y1) >= fh && mode >= 1)
					{
						if (mode == 2) sprintf_s(buf, sizeof(buf), "%.0fx%.0f", static_cast<double>(lv[q]->BidVolume), static_cast<double>(lv[q]->AskVolume));
						else sprintf_s(buf, sizeof(buf), "%+.0f", d);
						F.TextCenter(x, y1 + (y2 - y1 - fh) / 2, buf, inten > 0.55f ? T.bg : T.text);
					}
					if (imb[q] != 0)
					{
						const uint32_t ic = imb[q] == 1 ? T.bull : (imb[q] == -1 ? T.bear : T.gold);
						F.Box(x - half, y1, x + half, y2, stacked[q] ? Blend(ic, T.text, 0.35f) : ic, stacked[q] ? 2 : 1);
					}
					if (q == pocQ) F.Box(x - half - 1, y1 - 1, x + half + 1, y2 + 1, T.gold, mode == 0 ? 1 : 2);
				}
				if (yTop < yBot && i < static_cast<int>(S.dcs.dcs.size()))
				{
					const float v = S.dcs.dcs[i];
					const uint32_t bc = v >= weak ? T.bull : (v <= -weak ? T.bear : T.neutral);
					F.Box(x - half - 1, yTop - 1, x + half + 1, yBot + 1, forming ? Blend(bc, T.bg, 0.4f) : bc, 1);
				}
				if (mode >= 1)
				{
					// unfinished auctions: both sides traded at the extreme
					if (lv[m - 1]->BidVolume > 0 && lv[m - 1]->AskVolume > 0) { F.Font(Max(7, V.fontPt - 2), true); F.Text(x + half + 2, F.YOf(lv[m - 1]->PriceInTicks * F.tick + 0.5f * F.tick) - fh, "u", T.gold); F.Font(Max(7, V.fontPt - 2), false); }
					if (lv[0]->BidVolume > 0 && lv[0]->AskVolume > 0) { F.Font(Max(7, V.fontPt - 2), true); F.Text(x + half + 2, F.YOf(lv[0]->PriceInTicks * F.tick - 0.5f * F.tick) + 1, "u", T.gold); F.Font(Max(7, V.fontPt - 2), false); }
				}
				if (mode == 2 && i < static_cast<int>(S.flow.delta.size()))
				{
					// delta above, volume below
					const float dl = S.flow.delta[i];
					char t[24]; Abbrev(dl, t, sizeof(t)); if (dl > 0) sprintf_s(buf, sizeof(buf), "+%s", t); else strcpy_s(buf, sizeof(buf), t);
					F.TextCenter(x, F.YOf(sc.High[i]) - fh - 3, buf, dl >= 0 ? T.bull : T.bear);
					Abbrev(sc.Volume[i], buf, sizeof(buf));
					F.TextCenter(x, F.YOf(sc.Low[i]) + 3, buf, T.dim);
				}
			}
			F.Font(V.fontPt, false);
		}

		// ---------------- Docked volume profile (session, ghost prior, composite) ----------------
		void DrawProfile(Frame& F)
		{
			SCStudyInterfaceRef sc = F.sc; ChartState& S = F.S; const Theme& T = F.T; const VisualConfig& V = F.V;
			const AuctionState& A = S.auction; const AuctionParams& AP = S.params.auction;
			if (!F.fillVisible || F.fillW < 40) return;
			const int tpl = Max(1, AP.profileTicksPerLevel);
			const int W = Max(30, F.fillW * Clamp(V.profileWidthPct, 10, 90) / 100);
			const int x0 = V.profileDockRight ? F.fillRight - W : F.fillLeft + 3;
			const bool dockR = V.profileDockRight;
			typedef std::map<int, AuctionState::PLevel> PMap;

			struct Painter
			{
				static void Draw(Frame& F, const PMap& m, int x0, int W, int tpl, bool split, uint32_t single, int alphaIn, int alphaOut, int vaLo, int vaHi, bool outline, bool dockR)
				{
					if (m.empty()) return;
					double mx = 0; for (PMap::const_iterator it = m.begin(); it != m.end(); ++it) mx = Max(mx, it->second.vol);
					if (mx <= 0) return;
					const Theme& T = F.T;
					for (PMap::const_iterator it = m.begin(); it != m.end(); ++it)
					{
						const float pb = (static_cast<float>(it->first) * tpl - 0.5f) * F.tick, pt = (static_cast<float>(it->first) * tpl + tpl - 0.5f) * F.tick;
						int y1 = F.YOf(pt), y2 = F.YOf(pb);
						if (y2 < F.top || y1 > F.bottom) continue;
						if (y2 - y1 < 1) y2 = y1 + 1;
						const int w = static_cast<int>(W * it->second.vol / mx);
						if (w <= 0) continue;
						const bool inVa = it->first >= vaLo && it->first <= vaHi;
						const int a = inVa ? alphaIn : alphaOut;
						const int xs = dockR ? x0 + W - w : x0;
						if (split && it->second.vol > 0)
						{
							const int wb = static_cast<int>(w * it->second.bid / it->second.vol);
							F.Fill(xs, y1, xs + wb, y2, T.bear, a); F.Fill(xs + wb, y1, xs + w, y2, T.bull, a);
						}
						else if (outline) { F.Line(xs + w, y1, xs + w, y2, single, 1); }
						else F.Fill(xs, y1, xs + w, y2, single, a);
					}
				}
			};

			// composite (faint, behind)
			if (V.layer[L_COMPOSITE] && !A.sessionHist.empty())
			{
				PMap comp; const int days = Clamp(AP.compositeDays, 1, 30);
				for (int d = static_cast<int>(A.sessionHist.size()) - 1, c = 0; d >= 0 && c < days; --d, ++c)
					for (PMap::const_iterator it = A.sessionHist[d].begin(); it != A.sessionHist[d].end(); ++it) { AuctionState::PLevel& L = comp[it->first]; L.vol += it->second.vol; L.bid += it->second.bid; L.ask += it->second.ask; }
				Painter::Draw(F, comp, x0, W, tpl, false, Blend(T.bg, T.cyan, 0.5f), 10, 10, INT_MIN, INT_MAX, false, dockR);
			}
			// ghost prior session
			if (V.layer[L_GHOST] && !A.prevProf.empty())
			{ const int ga = A.profVol.empty() ? 34 : 16; Painter::Draw(F, A.prevProf, x0, W, tpl, false, T.dim, ga, ga, INT_MIN, INT_MAX, false, dockR); }
			// developing session, split by aggressor, value area brighter
			if (!A.profVol.empty())
			{
				int vaLo = INT_MIN, vaHi = INT_MAX;
				if (A.devVah > A.devVal) { vaLo = static_cast<int>(floor(A.devVal / F.tick + 0.5f + 0.5f)) / tpl; vaHi = static_cast<int>(floor(A.devVah / F.tick + 0.5f - 0.5f)) / tpl; }
				Painter::Draw(F, A.profVol, x0, W, tpl, true, 0, 62, 32, vaLo, vaHi, false, dockR);
				// POC / VAH / VAL markers with a gold POC pill
				if (A.devPoc > 0)
				{
					const int yp = F.YOf(A.devPoc);
					F.Line(x0, yp, x0 + W, yp, T.gold, 2);
					F.Font(V.fontPt - 1, true);
					char txt[32]; sprintf_s(txt, sizeof(txt), "POC %s", F.Px(A.devPoc));
					if (dockR) F.Pill(x0 - 4, yp - F.fontH / 2 - 1, txt, T.gold, T.bg, 100, true); else F.Pill(x0 + W + 4, yp - F.fontH / 2 - 1, txt, T.gold, T.bg, 100, false);
					F.Font(V.fontPt, false);
					const int yh = F.YOf(A.devVah), yl = F.YOf(A.devVal);
					F.Line(x0, yh, x0 + W, yh, T.cyan, 1, 2); F.Line(x0, yl, x0 + W, yl, T.cyan, 1, 2);
				}
			}
			else if (!A.prevProf.empty() && A.prevPoc > 0)
			{
				// pre-open: show the prior session's POC on the ghost
				const int yp = F.YOf(A.prevPoc);
				F.Line(x0, yp, x0 + W, yp, Blend(T.gold, T.dim, 0.5f), 1, 1);
			}
		}

		// ---------------- Order-flow tape (bottom strip) ----------------
		void DrawTape(Frame& F, int rowMask)
		{
			ChartState& S = F.S; const Theme& T = F.T; const VisualConfig& V = F.V;
			const FlowState& FL = S.flow; const DcsState& D = S.dcs;
			if (FL.delta.empty() || D.dcs.empty()) return;
			F.Clip(F.left, F.top, F.right, F.bottom);
			F.Fill(F.left, F.top, F.right, F.bottom, T.bg, 100);
			struct Row { const char* label; int kind; };
			static const Row rows[6] = { { "Dlt", 0 }, { "Vol", 1 }, { "Dl%", 2 }, { "CVD", 3 }, { "Imb", 4 }, { "DCS", 5 } };
			int active[6]; int nRows = 0;
			for (int r = 0; r < 6; ++r) if (rowMask & (1 << r)) active[nRows++] = r;
			if (nRows == 0) { F.Unclip(); return; }
			F.Font(Max(7, V.fontPt - 1), false);
			const int labelW = F.TextW("CVD") + 10;
			const int avail = F.bottom - F.top - 2;
			int rowH = avail / nRows;
			int shown = nRows;
			while (shown > 1 && rowH < F.fontH + 2) { --shown; rowH = avail / shown; }
			// per-row scale over the visible bars
			double mx[6] = { 1, 1, 1, 1, 1, 1 };
			for (int i = F.firstVis; i <= F.lastVis; ++i)
			{
				if (i >= static_cast<int>(FL.delta.size())) break;
				mx[0] = Max(mx[0], fabs(static_cast<double>(FL.delta[i]))); mx[1] = Max(mx[1], static_cast<double>(F.sc.Volume[i]));
				mx[3] = Max(mx[3], fabs(static_cast<double>(FL.cvdDz[i])));
			}
			mx[2] = 0.6; mx[4] = 3; mx[5] = 100;
			const int half = F.spacing / 2;
			const bool text = F.spacing >= 26;
			char buf[32];
			for (int r = 0; r < shown; ++r)
			{
				const int kind = rows[active[r]].kind;
				const int y0 = F.top + 1 + r * rowH, y1 = y0 + rowH - 1;
				for (int i = F.firstVis; i <= F.lastVis; ++i)
				{
					if (i >= static_cast<int>(FL.delta.size())) break;
					double v = 0; uint32_t pos = T.bull, neg = T.bear;
					switch (kind)
					{
					case 0: v = FL.delta[i]; break;
					case 1: v = F.sc.Volume[i]; pos = T.cyan; break;
					case 2: v = FL.deltaPct[i]; break;
					case 3: v = FL.cvdDz[i]; break;
					case 4: v = (FL.imbMark[i] == 2) ? 0 : (FL.imbMark[i] != 0 ? (FL.imbMark[i] > 0 ? 1 : -1) * Max(1, static_cast<int>(fabs(FL.fImb[i]) * 3)) : 0); break;
					case 5: v = D.dcs[i]; break;
					}
					const double inten = Clamp(pow(fabs(v) / mx[kind], 0.7), 0.0, 1.0);
					uint32_t c = Blend(T.bg, v >= 0 ? pos : neg, static_cast<float>(0.12 + 0.68 * inten));
					if (i == F.n - 1) c = Blend(c, T.bg, 0.4f);
					const int x = F.XOf(i);
					F.Fill(x - half, y0, x + half, y1, c, 100);
					if (text && rowH >= F.fontH)
					{
						if (kind == 2) sprintf_s(buf, sizeof(buf), "%+.0f", v * 100); else if (kind == 5) sprintf_s(buf, sizeof(buf), "%+.0f", v);
						else if (kind == 4) sprintf_s(buf, sizeof(buf), "%+d", static_cast<int>(v)); else Abbrev(v, buf, sizeof(buf));
						F.TextCenter(x, y0 + (rowH - F.fontH) / 2, buf, inten > 0.5 ? T.bg : T.text);
					}
				}
				F.Fill(F.left, y0, F.left + labelW, y1, T.panel, 100);
				F.Text(F.left + 4, y0 + (rowH - F.fontH) / 2, rows[active[r]].label, T.dim);
				F.Line(F.left, y1, F.right, y1, T.grid, 1);
			}
			F.Unclip();
		}
	} // namespace render

	// ---- GDI entry points -------------------------------------------------------
	namespace render_entry
	{
		inline double NowMs() { LARGE_INTEGER f, t; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&t); return f.QuadPart ? 1000.0 * t.QuadPart / f.QuadPart : 0.0; }
	}

} // namespace nqe

using namespace nqe;

// ==== 15 Study functions ======================================================

// Helper: colour input declaration
#define NQE_COLOR_INPUT(IDX, NAME, R, G, B) { sc.Input[IDX].Name = NAME; sc.Input[IDX].SetColor(RGB(R, G, B)); }
#define NQE_YESNO_INPUT(IDX, NAME, V)       { sc.Input[IDX].Name = NAME; sc.Input[IDX].SetYesNo(V); }
#define NQE_INT_INPUT(IDX, NAME, V, LO, HI) { sc.Input[IDX].Name = NAME; sc.Input[IDX].SetInt(V); sc.Input[IDX].SetIntLimits(LO, HI); }
#define NQE_FLT_INPUT(IDX, NAME, V, LO, HI) { sc.Input[IDX].Name = NAME; sc.Input[IDX].SetFloat(static_cast<float>(V)); sc.Input[IDX].SetFloatLimits(static_cast<float>(LO), static_cast<float>(HI)); }

// Diagnostic region numbers (used only when the Overlay's "Show Diagnostic Regions" is on)
static const int kDiagRegionFlow = 2, kDiagRegionInter = 3, kDiagRegionDcs = 4, kDiagRegionVal = 5;

// --- 1. Auction / Structure ---------------------------------------------------
enum AuctionInput
{
	AI_RTH_START = 0, AI_RTH_END, AI_ATR_LEN, AI_IB_MIN, AI_OPEN_MIN, AI_VA_PCT, AI_PROF_TICKS, AI_NAKED_N, AI_TPO_MIN,
	AI_SWING_N, AI_SWING_ATR, AI_EQ_TOL, AI_BOS_DECAY, AI_IBEXT_A, AI_IBEXT_B, AI_IBEXT_C, AI_COMPOSITE_DAYS, AI_ADR_DAYS,
	AI_D_DEVVA, AI_D_PDVA, AI_D_ON, AI_D_IB, AI_D_PDHL, AI_D_SWING, AI_D_BOS,
	AI_C_POC, AI_C_VA, AI_C_PD, AI_C_ON, AI_C_IB, AI_C_IBEXT, AI_C_SWH, AI_C_SWL, AI_C_BOS, AI_C_CHOCH, AI_COUNT
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
		sc.StudyDescription = "Developing/prior-day volume profile, naked POCs, single prints, ON range, IB, swings, BOS/CHoCH, liquidity pools. Owns the session and ATR settings for the whole suite. Level rays/pills are drawn by the Terminal Overlay.";
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
		sc.Subgraph[AS_SWH].LineWidth = 2; sc.Subgraph[AS_SWL].LineWidth = 2; sc.Subgraph[AS_BOSU].LineWidth = 2; sc.Subgraph[AS_BOSD].LineWidth = 2; sc.Subgraph[AS_CHU].LineWidth = 2; sc.Subgraph[AS_CHD].LineWidth = 2;

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
		NQE_INT_INPUT(AI_COMPOSITE_DAYS, "Profile: Composite Days (docked profile)", 5, 1, 30);
		NQE_INT_INPUT(AI_ADR_DAYS, "Session: ADR Lookback Days", 10, 1, 60);
		NQE_YESNO_INPUT(AI_D_DEVVA, "Lines: Developing POC/VAH/VAL", 1);
		NQE_YESNO_INPUT(AI_D_PDVA, "Lines: Prior Day POC/VAH/VAL", 1);
		NQE_YESNO_INPUT(AI_D_ON, "Lines: Overnight High/Low", 1);
		NQE_YESNO_INPUT(AI_D_IB, "Lines: IB and Extensions", 1);
		NQE_YESNO_INPUT(AI_D_PDHL, "Lines: Prior Day High/Low", 1);
		NQE_YESNO_INPUT(AI_D_SWING, "Marks: Swing Points", 0);
		NQE_YESNO_INPUT(AI_D_BOS, "Marks: BOS/CHoCH", 1);
		NQE_COLOR_INPUT(AI_C_POC, "Color: Developing POC", 255, 200, 87);
		NQE_COLOR_INPUT(AI_C_VA, "Color: Developing VAH/VAL", 62, 198, 255);
		NQE_COLOR_INPUT(AI_C_PD, "Color: Prior Day Levels", 92, 101, 119);
		NQE_COLOR_INPUT(AI_C_ON, "Color: Overnight High/Low", 90, 140, 200);
		NQE_COLOR_INPUT(AI_C_IB, "Color: Initial Balance", 230, 200, 60);
		NQE_COLOR_INPUT(AI_C_IBEXT, "Color: IB Extensions", 140, 120, 40);
		NQE_COLOR_INPUT(AI_C_SWH, "Color: Swing High", 255, 77, 94);
		NQE_COLOR_INPUT(AI_C_SWL, "Color: Swing Low", 0, 200, 150);
		NQE_COLOR_INPUT(AI_C_BOS, "Color: BOS", 230, 234, 242);
		NQE_COLOR_INPUT(AI_C_CHOCH, "Color: CHoCH", 255, 160, 0);
		return;
	}
	if (sc.LastCallToFunction) { Release(sc, E_AUCTION); return; }
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	ChartState& S = Acquire(sc);
	S.studyPresent[E_AUCTION] = true;
	if (sc.MaintainVolumeAtPriceData == 0) { sc.MaintainVolumeAtPriceData = 1; sc.FlagToReloadChartData = 1; }

	BaseParams bp{}; bp.rthStartSec = sc.Input[AI_RTH_START].GetTime(); bp.rthEndSec = sc.Input[AI_RTH_END].GetTime(); bp.atrLength = sc.Input[AI_ATR_LEN].GetInt();
	if (!S.terminalPresent) SetParams(S, E_BASE, S.params.base, bp);
	AuctionParams ap{};
	ap.ibMinutes = sc.Input[AI_IB_MIN].GetInt(); ap.openTypeMinutes = sc.Input[AI_OPEN_MIN].GetInt(); ap.valueAreaPct = sc.Input[AI_VA_PCT].GetFloat();
	ap.profileTicksPerLevel = sc.Input[AI_PROF_TICKS].GetInt(); ap.nakedPocsTracked = sc.Input[AI_NAKED_N].GetInt(); ap.tpoMinutes = sc.Input[AI_TPO_MIN].GetInt();
	ap.swingStrength = sc.Input[AI_SWING_N].GetInt(); ap.swingMinAtr = sc.Input[AI_SWING_ATR].GetFloat(); ap.equalTolTicks = sc.Input[AI_EQ_TOL].GetInt();
	ap.bosDecayBars = sc.Input[AI_BOS_DECAY].GetInt(); ap.ibExtA = sc.Input[AI_IBEXT_A].GetFloat(); ap.ibExtB = sc.Input[AI_IBEXT_B].GetFloat(); ap.ibExtC = sc.Input[AI_IBEXT_C].GetFloat();
	ap.compositeDays = sc.Input[AI_COMPOSITE_DAYS].GetInt(); ap.adrDays = sc.Input[AI_ADR_DAYS].GetInt();
	if (!S.terminalPresent) SetParams(S, E_AUCTION, S.params.auction, ap);

	CheckDataStamp(sc, S);
	CheckWarnings(sc, S);
	EnsureAuction(sc, S);

	int& gen = sc.GetPersistentInt(1);
	int start = Min(sc.UpdateStartIndex, S.auction.dirtyFrom); S.auction.dirtyFrom = INT_MAX;
	if (gen != S.auction.generation) { start = 0; gen = S.auction.generation; }
	if (start < 0) start = 0;
	const AuctionState& A = S.auction; const AuctionParams& P = S.params.auction;
	const bool levelsOn = S.vis.layer[L_LEVELS];
	const bool dDev = levelsOn && sc.Input[AI_D_DEVVA].GetYesNo() != 0, dPd = levelsOn && sc.Input[AI_D_PDVA].GetYesNo() != 0, dOn = levelsOn && sc.Input[AI_D_ON].GetYesNo() != 0, dIb = levelsOn && sc.Input[AI_D_IB].GetYesNo() != 0;
	const bool dPdhl = levelsOn && sc.Input[AI_D_PDHL].GetYesNo() != 0, dSw = sc.Input[AI_D_SWING].GetYesNo() != 0, dBos = sc.Input[AI_D_BOS].GetYesNo() != 0 && S.vis.preset != PRESET_CLEAN;
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
}

// --- 2. VWAP ------------------------------------------------------------------
enum VwapInput { VI_ANCHOR = 0, VI_B1, VI_B2, VI_B3, VI_SLOPE, VI_ACCEPT, VI_D_SESSION, VI_D_ON, VI_D_RTH, VI_D_SWING, VI_C_VWAP, VI_C_B1, VI_C_B2, VI_C_B3, VI_C_ON, VI_C_RTH, VI_C_SWH, VI_C_SWL, VI_COUNT };
enum VwapSubgraph { VS_VWAP = 0, VS_B1U, VS_B1D, VS_B2U, VS_B2D, VS_B3U, VS_B3D, VS_ON, VS_RTH, VS_SWH, VS_SWL, VS_F_SLOPE, VS_F_POS, VS_F_ACCEPT, VS_COUNT };

SCSFExport scsf_NQEdge_VWAP(SCStudyInterfaceRef sc)
{
	if (sc.SetDefaults)
	{
		sc.GraphName = "NQ Edge: VWAP Engine";
		sc.StudyDescription = "Session VWAP with variance bands, anchored VWAPs (overnight open, RTH open, last swing high/low), ATR-normalized slope and acceptance state. Band fills come from the Terminal Backdrop (VWAP cloud).";
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
		NQE_YESNO_INPUT(VI_D_SESSION, "Lines: Session VWAP + Bands", 1);
		NQE_YESNO_INPUT(VI_D_ON, "Lines: Overnight-anchored VWAP", 1);
		NQE_YESNO_INPUT(VI_D_RTH, "Lines: RTH-anchored VWAP", 0);
		NQE_YESNO_INPUT(VI_D_SWING, "Lines: Swing-anchored VWAPs", 0);
		NQE_COLOR_INPUT(VI_C_VWAP, "Color: VWAP", 255, 200, 87);
		NQE_COLOR_INPUT(VI_C_B1, "Color: Band 1", 190, 160, 70);
		NQE_COLOR_INPUT(VI_C_B2, "Color: Band 2", 140, 120, 55);
		NQE_COLOR_INPUT(VI_C_B3, "Color: Band 3", 92, 101, 119);
		NQE_COLOR_INPUT(VI_C_ON, "Color: AVWAP Overnight", 120, 170, 255);
		NQE_COLOR_INPUT(VI_C_RTH, "Color: AVWAP RTH", 255, 170, 90);
		NQE_COLOR_INPUT(VI_C_SWH, "Color: AVWAP Swing High", 255, 77, 94);
		NQE_COLOR_INPUT(VI_C_SWL, "Color: AVWAP Swing Low", 0, 200, 150);
		return;
	}
	if (sc.LastCallToFunction) { Release(sc, E_VWAP); return; }
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	ChartState& S = Acquire(sc);
	S.studyPresent[E_VWAP] = true;
	VwapParams vp{}; vp.anchor = sc.Input[VI_ANCHOR].GetIndex(); vp.band1 = sc.Input[VI_B1].GetFloat(); vp.band2 = sc.Input[VI_B2].GetFloat(); vp.band3 = sc.Input[VI_B3].GetFloat();
	vp.slopeBars = sc.Input[VI_SLOPE].GetInt(); vp.acceptCloses = sc.Input[VI_ACCEPT].GetInt();
	if (!S.terminalPresent) SetParams(S, E_VWAP, S.params.vwap, vp);
	CheckDataStamp(sc, S);
	EnsureVwap(sc, S);

	sc.Subgraph[VS_VWAP].PrimaryColor = sc.Input[VI_C_VWAP].GetColor();
	sc.Subgraph[VS_B1U].PrimaryColor = sc.Subgraph[VS_B1D].PrimaryColor = sc.Input[VI_C_B1].GetColor();
	sc.Subgraph[VS_B2U].PrimaryColor = sc.Subgraph[VS_B2D].PrimaryColor = sc.Input[VI_C_B2].GetColor();
	sc.Subgraph[VS_B3U].PrimaryColor = sc.Subgraph[VS_B3D].PrimaryColor = sc.Input[VI_C_B3].GetColor();
	sc.Subgraph[VS_ON].PrimaryColor = sc.Input[VI_C_ON].GetColor(); sc.Subgraph[VS_RTH].PrimaryColor = sc.Input[VI_C_RTH].GetColor();
	sc.Subgraph[VS_SWH].PrimaryColor = sc.Input[VI_C_SWH].GetColor(); sc.Subgraph[VS_SWL].PrimaryColor = sc.Input[VI_C_SWL].GetColor();
	const bool cloud = S.vis.layer[L_CLOUD];                     // the backdrop draws band fills; hide the band lines then
	const bool clean = S.vis.preset == PRESET_CLEAN;
	const bool dS = sc.Input[VI_D_SESSION].GetYesNo() != 0, dO = sc.Input[VI_D_ON].GetYesNo() != 0 && !clean, dR = sc.Input[VI_D_RTH].GetYesNo() != 0 && !clean, dW = sc.Input[VI_D_SWING].GetYesNo() != 0 && !clean;
	sc.Subgraph[VS_VWAP].DrawStyle = dS ? DRAWSTYLE_LINE : DRAWSTYLE_IGNORE;
	for (int k = VS_B1U; k <= VS_B3D; ++k) sc.Subgraph[k].DrawStyle = (dS && !cloud && !clean) ? DRAWSTYLE_LINE : DRAWSTYLE_IGNORE;
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
	FI_C_CVD_UP, FI_C_CVD_DN, FI_COUNT
};
enum FlowSubgraph { FS_CVD = 0, FS_DELTA, FS_DELTA_PCT, FS_VOLZ, FS_CVDZ, FS_F_ABS, FS_F_EXH, FS_F_IMB, FS_F_TRAP, FS_F_DIV, FS_F_LARGE, FS_COUNT };

SCSFExport scsf_NQEdge_OrderFlow(SCStudyInterfaceRef sc)
{
	if (sc.SetDefaults)
	{
		sc.GraphName = "NQ Edge: Order Flow Engine";
		sc.StudyDescription = "Tick-level delta/CVD, CVD divergence, absorption, exhaustion, stacked imbalances, large trades (T&S + VAP), trapped traders, swing legs. Visuals are drawn by the Terminal studies; the CVD line appears only in the diagnostic region.";
		sc.GraphRegion = 0; sc.AutoLoop = 0; sc.CalculationPrecedence = STD_PREC_LEVEL; sc.ValueFormat = 0; sc.MaintainVolumeAtPriceData = 1; sc.DrawZeros = 0;
		const char* names[FS_COUNT] = { "CVD", "Delta", "Delta %", "Volume Z", "CVD Z", "f.absorb", "f.exhaust", "f.imbalance", "f.trapped", "f.cvdDiv", "f.largeTrade" };
		for (int k = 0; k < FS_COUNT; ++k) { sc.Subgraph[k].Name = names[k]; sc.Subgraph[k].DrawStyle = DRAWSTYLE_IGNORE; sc.Subgraph[k].DrawZeros = 1; }
		sc.Subgraph[FS_CVD].LineWidth = 2; sc.Subgraph[FS_CVD].PrimaryColor = RGB(0, 200, 150); sc.Subgraph[FS_CVD].SecondaryColor = RGB(255, 77, 94); sc.Subgraph[FS_CVD].SecondaryColorUsed = 1;
		sc.Subgraph[FS_DELTA].PrimaryColor = RGB(90, 90, 90);

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
		NQE_INT_INPUT(FI_LT_MAX, "Large Trade: Max Bubbles Kept", 100, 0, 500);
		NQE_INT_INPUT(FI_TRAP_LOOK, "Trapped: Breakout Lookback Bars", 20, 3, 200);
		NQE_INT_INPUT(FI_TRAP_K, "Trapped: Reversal Within K Bars", 3, 1, 20);
		NQE_FLT_INPUT(FI_TRAP_DELTA, "Trapped: Min Breakout Delta %", 20.0, 0.0, 100.0);
		NQE_FLT_INPUT(FI_DIV_ATR, "Divergence: Min Swing Distance (ATR)", 0.5, 0.0, 10.0);
		NQE_INT_INPUT(FI_DECAY, "Event Decay Bars (feature half-life)", 8, 1, 100);
		NQE_INT_INPUT(FI_MAX_ZONES, "Max Active Zones Kept", 30, 1, 200);
		NQE_COLOR_INPUT(FI_C_CVD_UP, "Color: CVD Up (diagnostic)", 0, 200, 150);
		NQE_COLOR_INPUT(FI_C_CVD_DN, "Color: CVD Down (diagnostic)", 255, 77, 94);
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
	if (!S.terminalPresent) SetParams(S, E_FLOW, S.params.flow, fp);
	CheckDataStamp(sc, S);
	EnsureFlow(sc, S);

	const bool diag = S.vis.diagnostics;
	sc.GraphRegion = diag ? kDiagRegionFlow : 0;
	sc.Subgraph[FS_CVD].DrawStyle = diag ? DRAWSTYLE_LINE : DRAWSTYLE_IGNORE;
	sc.Subgraph[FS_DELTA].DrawStyle = diag ? DRAWSTYLE_BAR : DRAWSTYLE_IGNORE;
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
}

// --- 4. Regime + MTF ----------------------------------------------------------
enum RegimeInput { RI_ER_LEN = 0, RI_ER_TREND, RI_ER_BAL, RI_ATR_FAST, RI_ATR_SLOW, RI_CHOP, RI_IB_DAYS, RI_INSIDE, RI_HYST, RI_MTF_EMA, RI_MTF_SWING, RI_SHADE, RI_C_UP, RI_C_DN, RI_C_BAL, RI_C_CHOP, RI_COUNT };
enum RegimeSubgraph { RS_BG = 0, RS_REGIME, RS_ER, RS_ATR_RATIO, RS_TREND, RS_MTF1, RS_MTF5, RS_MTF15, RS_MTF60, RS_MTF_BIAS, RS_COUNT };

SCSFExport scsf_NQEdge_Regime(SCStudyInterfaceRef sc)
{
	if (sc.SetDefaults)
	{
		sc.GraphName = "NQ Edge: Regime + MTF Bias";
		sc.StudyDescription = "Classifies every closed bar as Trend Up / Trend Down / Balance / Volatile Chop (with hysteresis) and computes 1m/5m/15m/60m trend bias. The regime tint is drawn by the Terminal Backdrop; the opaque shade here is diagnostic.";
		sc.GraphRegion = 0; sc.AutoLoop = 0; sc.CalculationPrecedence = LOW_PREC_LEVEL; sc.ValueFormat = 2; sc.ScaleRangeType = SCALE_INDEPENDENT; sc.DrawZeros = 0; sc.DrawStudyUnderneathMainPriceGraph = 1;
		const char* names[RS_COUNT] = { "Regime Shade (diagnostic)", "Regime", "Efficiency Ratio", "ATR Ratio", "f.regimeTrend", "MTF 1m", "MTF 5m", "MTF 15m", "MTF 60m", "f.mtfBias" };
		for (int k = 0; k < RS_COUNT; ++k) { sc.Subgraph[k].Name = names[k]; sc.Subgraph[k].DrawStyle = DRAWSTYLE_IGNORE; sc.Subgraph[k].DrawZeros = 0; }
		sc.Subgraph[RS_BG].PrimaryColor = RGB(20, 40, 20);
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
		NQE_YESNO_INPUT(RI_SHADE, "Opaque Regime Shade (diagnostic)", 0);
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
	if (!S.terminalPresent) SetParams(S, E_REGIME, S.params.regime, rp);
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
		sc.StudyDescription = "Relative strength vs YM/ES/RTY, SMT divergence, NYSE TICK (cumulative, extremes, divergence), mega-cap leadership breadth, lead/lag. Every chart number is optional (0 = off). Set the chart numbers from the title bars of the other charts in this chartbook.";
		sc.GraphRegion = 0; sc.AutoLoop = 0; sc.CalculationPrecedence = LOW_PREC_LEVEL; sc.ValueFormat = 2; sc.DrawZeros = 1;
		const char* names[IS_COUNT] = { "RS vs YM", "RS vs ES", "RS vs RTY", "f.rsIndex", "f.smt", "f.tickCum", "f.tickExt", "f.tickDiv", "f.megaCap", "Intermarket Composite" };
		for (int k = 0; k < IS_COUNT; ++k) { sc.Subgraph[k].Name = names[k]; sc.Subgraph[k].DrawStyle = DRAWSTYLE_IGNORE; sc.Subgraph[k].DrawZeros = 1; }
		sc.Subgraph[IS_RS_INDEX].PrimaryColor = RGB(120, 180, 255); sc.Subgraph[IS_MEGA].PrimaryColor = RGB(255, 200, 80); sc.Subgraph[IS_COMPOSITE].PrimaryColor = RGB(90, 90, 90);
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
		NQE_COLOR_INPUT(II_C_RS, "Color: RS Line (diagnostic)", 120, 180, 255);
		NQE_COLOR_INPUT(II_C_BREADTH, "Color: Leadership Breadth (diagnostic)", 255, 200, 80);
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
	if (!S.terminalPresent) SetParams(S, E_INTER, S.params.inter, ip);
	CheckDataStamp(sc, S);
	EnsureInter(sc, S);
	const bool diag = S.vis.diagnostics;
	sc.GraphRegion = diag ? kDiagRegionInter : 0;
	sc.Subgraph[IS_RS_INDEX].DrawStyle = diag ? DRAWSTYLE_LINE : DRAWSTYLE_IGNORE; sc.Subgraph[IS_MEGA].DrawStyle = diag ? DRAWSTYLE_LINE : DRAWSTYLE_IGNORE; sc.Subgraph[IS_COMPOSITE].DrawStyle = diag ? DRAWSTYLE_BAR : DRAWSTYLE_IGNORE;
	sc.Subgraph[IS_RS_INDEX].PrimaryColor = sc.Input[II_C_RS].GetColor(); sc.Subgraph[IS_MEGA].PrimaryColor = sc.Input[II_C_BREADTH].GetColor();
	int& gen = sc.GetPersistentInt(1);
	int start = Min(sc.UpdateStartIndex, S.inter.dirtyFrom); S.inter.dirtyFrom = INT_MAX;
	if (gen != S.inter.generation) { start = 0; gen = S.inter.generation; }
	if (start < 0) start = 0;
	const InterState& I = S.inter;
	for (int i = start; i < sc.ArraySize; ++i)
	{
		sc.Subgraph[IS_RS_YM][i] = I.rsYM[i]; sc.Subgraph[IS_RS_ES][i] = I.rsES[i]; sc.Subgraph[IS_RS_RTY][i] = I.rsRTY[i]; sc.Subgraph[IS_RS_INDEX][i] = IsNan(I.rsIndex[i]) ? 0.0f : I.rsIndex[i];
		sc.Subgraph[IS_SMT][i] = IsNan(I.smt[i]) ? 0.0f : I.smt[i]; sc.Subgraph[IS_TICK_CUM][i] = IsNan(I.tickCum[i]) ? 0.0f : I.tickCum[i]; sc.Subgraph[IS_TICK_EXT][i] = IsNan(I.tickExt[i]) ? 0.0f : I.tickExt[i]; sc.Subgraph[IS_TICK_DIV][i] = IsNan(I.tickDiv[i]) ? 0.0f : I.tickDiv[i];
		sc.Subgraph[IS_MEGA][i] = IsNan(I.megaCap[i]) ? 0.0f : I.megaCap[i]; sc.Subgraph[IS_COMPOSITE][i] = I.composite[i];
	}
}

// --- 6. DCS -------------------------------------------------------------------
enum DcsInput
{
	DI_FILE = 0, DI_RELOAD, DI_THR, DI_FADE, DI_SMOOTH, DI_STRONG, DI_WEAK, DI_S1, DI_S2, DI_S3, DI_S4, DI_S5, DI_MTF, DI_TOL, DI_STOPBUF, DI_MINRR, DI_MINTGT, DI_ALERTS, DI_SOUND, DI_ALERT_GRADE,
	DI_C_GREEN, DI_C_RED, DI_C_GRAY, DI_C_SMOOTH, DI_C_THR, DI_COUNT
};
enum DcsSubgraph { DS_DCS = 0, DS_SMOOTH, DS_THR_UP, DS_THR_DN, DS_SIGNAL, DS_BIAS, DS_COUNT };

SCSFExport scsf_NQEdge_DCS(SCStudyInterfaceRef sc)
{
	if (sc.SetDefaults)
	{
		sc.GraphName = "NQ Edge: Directional Conviction Score";
		sc.StudyDescription = "Fuses all engine features into a regime-gated score (-100..+100), detects and grades the five trade setups with structural stops and liquidity targets, and alerts them. The DCS ribbon, signal cards and R/R boxes are drawn by the Terminal Overlay; the histogram appears only in the diagnostic region.";
		sc.GraphRegion = 0; sc.AutoLoop = 0; sc.CalculationPrecedence = VERY_LOW_PREC_LEVEL; sc.ValueFormat = 0; sc.DrawZeros = 1;
		const char* names[DS_COUNT] = { "DCS", "DCS Smoothed", "+Threshold", "-Threshold", "Signal", "Bias" };
		for (int k = 0; k < DS_COUNT; ++k) { sc.Subgraph[k].Name = names[k]; sc.Subgraph[k].DrawZeros = 1; sc.Subgraph[k].DrawStyle = DRAWSTYLE_IGNORE; }
		sc.Subgraph[DS_DCS].PrimaryColor = RGB(128, 128, 128); sc.Subgraph[DS_DCS].LineWidth = 2;
		sc.Subgraph[DS_SMOOTH].PrimaryColor = RGB(255, 255, 255); sc.Subgraph[DS_SMOOTH].LineWidth = 1;
		sc.Subgraph[DS_THR_UP].PrimaryColor = RGB(90, 90, 90); sc.Subgraph[DS_THR_DN].PrimaryColor = RGB(90, 90, 90);
		sc.Input[DI_FILE].Name = "Weights File Name (in Data folder)"; sc.Input[DI_FILE].SetString("NQEdge_weights.txt");
		NQE_INT_INPUT(DI_RELOAD, "Weights Hot-Reload Check Seconds", 5, 1, 600);
		NQE_FLT_INPUT(DI_THR, "Signal Threshold |DCS| (0 = from weights file, default 40)", 0.0, 0.0, 100.0);
		NQE_FLT_INPUT(DI_FADE, "Fade Threshold In Balance |DCS| (0 = from weights file, default 20)", 0.0, 0.0, 100.0);
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
		NQE_YESNO_INPUT(DI_ALERTS, "Alerts Enabled", 1);
		sc.Input[DI_SOUND].Name = "Alert Sound Number"; sc.Input[DI_SOUND].SetAlertSoundNumber(1);
		sc.Input[DI_ALERT_GRADE].Name = "Alert Minimum Grade"; sc.Input[DI_ALERT_GRADE].SetCustomInputStrings("A only;A and B;All"); sc.Input[DI_ALERT_GRADE].SetCustomInputIndex(1);
		NQE_COLOR_INPUT(DI_C_GREEN, "Color: Deep Green (diagnostic histogram)", 0, 200, 150);
		NQE_COLOR_INPUT(DI_C_RED, "Color: Deep Red (diagnostic histogram)", 255, 77, 94);
		NQE_COLOR_INPUT(DI_C_GRAY, "Color: Neutral Gray (diagnostic histogram)", 138, 147, 166);
		NQE_COLOR_INPUT(DI_C_SMOOTH, "Color: Smoothed Line (diagnostic)", 255, 255, 255);
		NQE_COLOR_INPUT(DI_C_THR, "Color: Threshold Lines (diagnostic)", 90, 90, 90);
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
	dp.minTargetAtr = sc.Input[DI_MINTGT].GetFloat(); dp.maxSignalsDrawn = 30; dp.targetLineBars = 20;
	dp.alertsOn = sc.Input[DI_ALERTS].GetYesNo(); dp.alertSound = sc.Input[DI_SOUND].GetAlertSoundNumber(); dp.alertMinGrade = sc.Input[DI_ALERT_GRADE].GetIndex() + 1;
	if (!S.terminalPresent) SetParams(S, E_DCS, S.params.dcs, dp);
	CheckDataStamp(sc, S);
	EnsureDcs(sc, S);

	const bool diag = S.vis.diagnostics;
	sc.GraphRegion = diag ? kDiagRegionDcs : 0;
	sc.Subgraph[DS_DCS].DrawStyle = diag ? DRAWSTYLE_BAR : DRAWSTYLE_IGNORE; sc.Subgraph[DS_SMOOTH].DrawStyle = diag ? DRAWSTYLE_LINE : DRAWSTYLE_IGNORE;
	sc.Subgraph[DS_THR_UP].DrawStyle = sc.Subgraph[DS_THR_DN].DrawStyle = diag ? DRAWSTYLE_DASH : DRAWSTYLE_IGNORE;
	sc.Subgraph[DS_SMOOTH].PrimaryColor = sc.Input[DI_C_SMOOTH].GetColor(); sc.Subgraph[DS_THR_UP].PrimaryColor = sc.Subgraph[DS_THR_DN].PrimaryColor = sc.Input[DI_C_THR].GetColor();
	const uint32_t cG = sc.Input[DI_C_GREEN].GetColor(), cR = sc.Input[DI_C_RED].GetColor(), cN = sc.Input[DI_C_GRAY].GetColor();
	int& gen = sc.GetPersistentInt(1);
	int start = Min(sc.UpdateStartIndex, S.dcs.dirtyFrom); S.dcs.dirtyFrom = INT_MAX;
	if (gen != S.dcs.generation) { start = 0; gen = S.dcs.generation; }
	if (start < 0) start = 0;
	const DcsState& D = S.dcs;
	const float thrLine = S.params.dcs.signalThr > 0 ? S.params.dcs.signalThr : D.weights.thrSignal;
	for (int i = start; i < sc.ArraySize; ++i)
	{
		const float v = D.dcs[i];
		sc.Subgraph[DS_DCS][i] = v; sc.Subgraph[DS_SMOOTH][i] = D.dcsSmooth[i];
		sc.Subgraph[DS_THR_UP][i] = thrLine; sc.Subgraph[DS_THR_DN][i] = -thrLine;
		const float t = Clamp(static_cast<float>(fabs(v)) / 100.0f, 0.0f, 1.0f);
		uint32_t c = v >= 0 ? sc.RGBInterpolate(cN, cG, t) : sc.RGBInterpolate(cN, cR, t);
		if (i == sc.ArraySize - 1) c = sc.RGBInterpolate(c, RGB(0, 0, 0), 0.5f);
		sc.Subgraph[DS_DCS].DataColor[i] = c;
		sc.Subgraph[DS_SIGNAL][i] = static_cast<float>(D.signalType[i]) * static_cast<float>(D.signalDir[i]);
		sc.Subgraph[DS_BIAS][i] = static_cast<float>(D.barState[i]);
	}
	// alert only for a signal on the newest closed bar, only in real time, only at or above the minimum grade
	DcsState& DW = S.dcs;
	const int total = static_cast<int>(DW.signals.size());
	if (sc.Input[DI_ALERTS].GetYesNo() && total > 0 && !sc.IsFullRecalculation && sc.DownloadingHistoricalData == 0 && !sc.IsReplayRunning())
	{
		const Signal& g = DW.signals[total - 1];
		if (g.idx == sc.ArraySize - 2 && DW.lastAlertIdx != g.idx && g.grade <= S.params.dcs.alertMinGrade)
		{
			DW.lastAlertIdx = g.idx;
			SCString msg; msg.Format("NQ Edge %s @ %s stop %s T1 %s T2 %s", g.label, sc.FormatGraphValue(g.entry, sc.BaseGraphValueFormat).GetChars(),
				sc.FormatGraphValue(g.stop, sc.BaseGraphValueFormat).GetChars(), sc.FormatGraphValue(g.t1, sc.BaseGraphValueFormat).GetChars(), sc.FormatGraphValue(g.t2, sc.BaseGraphValueFormat).GetChars());
			const int snd = sc.Input[DI_SOUND].GetAlertSoundNumber();
			if (snd > 0) sc.SetAlert(snd - 1, g.idx, msg); else sc.AddAlertLine(msg, 1);
		}
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
		sc.StudyDescription = "Replays every DCS signal forward on closed bars (1-tick slippage, stop-first) and reports outcomes by setup, regime and grade to the HUD and projection arrow. The cumulative-R curve appears only in the diagnostic region.";
		sc.GraphRegion = 0; sc.AutoLoop = 0; sc.CalculationPrecedence = VERY_LOW_PREC_LEVEL; sc.ValueFormat = 2; sc.DrawZeros = 1;
		sc.Subgraph[VLS_CUMR].Name = "Cumulative R"; sc.Subgraph[VLS_CUMR].DrawStyle = DRAWSTYLE_IGNORE; sc.Subgraph[VLS_CUMR].PrimaryColor = RGB(62, 198, 255); sc.Subgraph[VLS_CUMR].LineWidth = 2;
		sc.Subgraph[VLS_RESULT].Name = "Signal Result (R)"; sc.Subgraph[VLS_RESULT].DrawStyle = DRAWSTYLE_IGNORE; sc.Subgraph[VLS_RESULT].PrimaryColor = RGB(255, 255, 255); sc.Subgraph[VLS_RESULT].LineWidth = 4; sc.Subgraph[VLS_RESULT].DrawZeros = 0;
		NQE_INT_INPUT(VLI_SLIP, "Slippage Ticks (entry and stop)", 1, 0, 20);
		NQE_INT_INPUT(VLI_MAXBARS, "Max Bars To Resolution", 120, 5, 2000);
		NQE_YESNO_INPUT(VLI_STOPFIRST, "Assume Stop First When Bar Hits Both", 1);
		NQE_INT_INPUT(VLI_MINSAMPLE, "Min Sample Size (warn below)", 30, 1, 1000);
		NQE_COLOR_INPUT(VLI_C_CURVE, "Color: Cumulative R (diagnostic)", 62, 198, 255);
		NQE_COLOR_INPUT(VLI_C_MARK, "Color: Result Marker (diagnostic)", 255, 255, 255);
		return;
	}
	if (sc.LastCallToFunction) { Release(sc, E_VAL); return; }
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	ChartState& S = Acquire(sc);
	S.studyPresent[E_VAL] = true;
	ValParams vp{}; vp.slippageTicks = sc.Input[VLI_SLIP].GetInt(); vp.maxBars = sc.Input[VLI_MAXBARS].GetInt(); vp.stopFirst = sc.Input[VLI_STOPFIRST].GetYesNo(); vp.minSample = sc.Input[VLI_MINSAMPLE].GetInt();
	if (!S.terminalPresent) SetParams(S, E_VAL, S.params.val, vp);
	CheckDataStamp(sc, S);
	EnsureVal(sc, S);
	const bool diag = S.vis.diagnostics;
	sc.GraphRegion = diag ? kDiagRegionVal : 0;
	sc.Subgraph[VLS_CUMR].DrawStyle = diag ? DRAWSTYLE_LINE : DRAWSTYLE_IGNORE; sc.Subgraph[VLS_RESULT].DrawStyle = diag ? DRAWSTYLE_POINT : DRAWSTYLE_IGNORE;
	sc.Subgraph[VLS_CUMR].PrimaryColor = sc.Input[VLI_C_CURVE].GetColor(); sc.Subgraph[VLS_RESULT].PrimaryColor = sc.Input[VLI_C_MARK].GetColor();
	int& gen = sc.GetPersistentInt(1);
	int start = Min(sc.UpdateStartIndex, S.val.dirtyFrom); S.val.dirtyFrom = INT_MAX;
	if (gen != S.val.generation) { start = 0; gen = S.val.generation; }
	if (start < 0) start = 0;
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
		sc.StudyDescription = "Writes one CSV row per closed bar (OHLCV, every engine feature, DCS, regime, setup flags + grade, forward returns at +5/+15/+30/+60 min, MFE/MAE) to the Data folder for the Python research loop.";
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
	if (!S.terminalPresent) SetParams(S, E_LOG, S.params.log, lp);
	CheckDataStamp(sc, S);
	EnsureLog(sc, S);
	if (sc.ArraySize > 0) sc.Subgraph[0][sc.ArraySize - 1] = static_cast<float>(S.log.rowsWritten);
}

// --- 9. NQ Edge Terminal: the one study ---------------------------------------
// Draws only in the price region with Sierra-native objects managed by line number (deleted and
// re-adjusted on every update). The docked profile and the calculated-values strip use GDI.
enum TermLayer { TL_CANDLES = 0, TL_BAND, TL_LEVELS, TL_SIGNALS, TL_HUD, TL_PROFILE, TL_ZONES, TL_BUBBLES, TL_SWING, TL_NOTES, TL_PROJ, TL_TAPE, TL_FIB, TL_NUMBERS, TL_CHANNEL, TL_FOOTPRINT, TL_KEYTIMES, TL_TRENDLINES, TL_COUNT };
static const char* kTermLayerNames[TL_COUNT] = { "Bias Candles", "VWAP + 1 Sigma Band", "Nearest Levels (pills)", "Signal Arrows + Boxes", "HUD", "Volume Profile (docked right)", "Zones", "Bubbles (large prints + absorption)", "Swing Delta Numbers", "Event Log + Markers", "Projection Arrow", "Calculated-Values Strip", "Fib Levels (last leg)", "Delta Per Bar", "Regression Channel", "Footprint Cells (bid x ask)", "Key Session Times (open / IB / close)", "Trend Lines (auto, from swings)" };
static const unsigned char kTermPreset[2][TL_COUNT] =
{
	{ 1, 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 1 },   // CLEAN
	{ 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1 },   // PRO
};
static const char* kOpenShort[8] = { "--", "Open-Drive up", "Open-Drive down", "Test-Drive up", "Test-Drive down", "Reject-Reverse up", "Reject-Reverse down", "Open-Auction" };
enum TermInput
{
	TI_PRESET = 0, TI_RTH_START, TI_RTH_END, TI_DAY_START, TI_ATR_LEN, TI_VA_PCT, TI_IB_MIN, TI_SWING_N, TI_SWING_ATR, TI_IMB_RATIO, TI_IMB_STACK, TI_ABS_Z,
	TI_C_YM, TI_C_ES, TI_C_TICK, TI_C_MEGA1, TI_C_MEGA2, TI_C_MEGA3,
	TI_WEIGHTS, TI_THR, TI_STRONG, TI_WEAK, TI_S1, TI_S2, TI_S3, TI_S4, TI_S5, TI_MIN_GRADE, TI_ALERTS, TI_SOUND, TI_LOG, TI_VWAP_ANCHOR,
	TI_RISK, TI_ETH, TI_MIN_STOP, TI_MAX_STOP, TI_MIN_RR, TI_DAY_LOSS, TI_MAX_TRADES, TI_TRADE_START, TI_TRADE_END,
	TI_LAYER0,                                   // TL_COUNT tri-state inputs follow
	TI_FONT = TI_LAYER0 + TL_COUNT, TI_NUM_BARS, TI_PROFILE_W, TI_BAND_STYLE, TI_BAND_ALPHA, TI_TL_SWINGS,
	TI_COL_BULL, TI_COL_BEAR, TI_COL_NEUTRAL, TI_COL_NEUTRAL_UP, TI_COL_VWAP, TI_COL_LEVEL, TI_COL_LONG, TI_COL_SHORT, TI_COL_TEXT, TI_COL_PANEL, TI_COL_DIM, TI_COUNT
};
enum TermSubgraph
{
	TS_CANDLE = 0, TS_CANDLE_FORMING, TS_VWAP, TS_BAND_TOP, TS_BAND_BOT, TS_ARROW_UP, TS_ARROW_DN,
	TS_H_DCS, TS_H_DCS_SMOOTH, TS_H_REGIME, TS_H_MTF, TS_H_CVD, TS_H_DELTA, TS_H_STRUCT, TS_H_VAPOS, TS_H_POC, TS_H_VAH, TS_H_VAL, TS_H_SIGNAL, TS_H_GRADE, TS_COUNT
};

namespace term
{
	using namespace nqe;

	// ---- drawing slots: every managed object has a persistent line number; unused slots are deleted each update ----
	inline void SlotText(SCStudyInterfaceRef sc, DrawSlot& s, int beginIndex, double relX, float value, bool relativeY, const char* text, uint32_t color, int fontSize, bool bold, uint32_t backColor, bool opaqueBack, int align)
	{
		if (s.line != 0 && sc.ChartDrawingExists(sc.ChartNumber, s.line) == 0) s.line = 0;
		s_UseTool T; T.ChartNumber = sc.ChartNumber; T.DrawingType = DRAWING_TEXT; T.Region = 0; T.AddMethod = UTAM_ADD_OR_ADJUST;
		if (s.line != 0) T.LineNumber = s.line;
		if (beginIndex >= 0) T.BeginIndex = beginIndex; else T.BeginDateTime = relX;
		T.BeginValue = value; T.UseRelativeVerticalValues = relativeY ? 1 : 0;
		T.Text = text; T.Color = color; T.FontSize = fontSize; T.FontBold = bold ? 1 : 0; T.FontFace = sc.GetChartTextFontFaceName();
		T.TransparentLabelBackground = opaqueBack ? 0 : 1; T.FontBackColor = backColor; T.TextAlignment = align; T.ReverseTextColor = 0;
		if (sc.UseTool(T) > 0) s.line = T.LineNumber;
		s.used = true;
	}
	inline void SlotLine(SCStudyInterfaceRef sc, DrawSlot& s, int i1, int i2, float v1, float v2, uint32_t color, int width, int style)
	{
		if (s.line != 0 && sc.ChartDrawingExists(sc.ChartNumber, s.line) == 0) s.line = 0;
		s_UseTool T; T.ChartNumber = sc.ChartNumber; T.DrawingType = DRAWING_LINE; T.Region = 0; T.AddMethod = UTAM_ADD_OR_ADJUST;
		if (s.line != 0) T.LineNumber = s.line;
		T.BeginIndex = i1; T.EndIndex = i2; T.BeginValue = v1; T.EndValue = v2; T.Color = color; T.LineWidth = static_cast<uint16_t>(width); T.LineStyle = static_cast<SubgraphLineStyles>(style);
		if (sc.UseTool(T) > 0) s.line = T.LineNumber;
		s.used = true;
	}
	inline void SlotRect(SCStudyInterfaceRef sc, DrawSlot& s, int i1, int i2, float top, float bottom, uint32_t color, int transparency, const char* text)
	{
		if (s.line != 0 && sc.ChartDrawingExists(sc.ChartNumber, s.line) == 0) s.line = 0;
		s_UseTool T; T.ChartNumber = sc.ChartNumber; T.DrawingType = DRAWING_RECTANGLEHIGHLIGHT; T.Region = 0; T.AddMethod = UTAM_ADD_OR_ADJUST;
		if (s.line != 0) T.LineNumber = s.line;
		T.BeginIndex = i1; T.EndIndex = Max(i1, i2); T.BeginValue = top; T.EndValue = bottom; T.Color = color; T.SecondaryColor = color; T.LineWidth = 1; T.TransparencyLevel = transparency;
		if (text && text[0]) { T.Text = text; T.FontSize = 8; T.TextAlignment = DT_LEFT | DT_TOP; }
		if (sc.UseTool(T) > 0) s.line = T.LineNumber;
		s.used = true;
	}
	inline void SlotMarker(SCStudyInterfaceRef sc, DrawSlot& s, int idx, float value, int type, int size, uint32_t color)
	{
		if (s.line != 0 && sc.ChartDrawingExists(sc.ChartNumber, s.line) == 0) s.line = 0;
		s_UseTool T; T.ChartNumber = sc.ChartNumber; T.DrawingType = DRAWING_MARKER; T.Region = 0; T.AddMethod = UTAM_ADD_OR_ADJUST;
		if (s.line != 0) T.LineNumber = s.line;
		T.BeginIndex = idx; T.BeginValue = value; T.MarkerType = type; T.MarkerSize = size; T.Color = color; T.LineWidth = static_cast<uint16_t>(Max(1, size / 2));
		if (sc.UseTool(T) > 0) s.line = T.LineNumber;
		s.used = true;
	}
	inline void SlotVLine(SCStudyInterfaceRef sc, DrawSlot& s, int idx, uint32_t color, int width, int style)
	{
		if (s.line != 0 && sc.ChartDrawingExists(sc.ChartNumber, s.line) == 0) s.line = 0;
		s_UseTool T; T.ChartNumber = sc.ChartNumber; T.DrawingType = DRAWING_VERTICALLINE; T.Region = 0; T.AddMethod = UTAM_ADD_OR_ADJUST;
		if (s.line != 0) T.LineNumber = s.line;
		T.BeginIndex = idx; T.Color = color; T.LineWidth = static_cast<uint16_t>(width); T.LineStyle = static_cast<SubgraphLineStyles>(style);
		if (sc.UseTool(T) > 0) s.line = T.LineNumber;
		s.used = true;
	}
	inline void SlotFlush(SCStudyInterfaceRef sc, DrawSlot* slots, int n, bool deleteAll)
	{
		for (int k = 0; k < n; ++k)
		{
			if ((!slots[k].used || deleteAll) && slots[k].line != 0) { sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, slots[k].line); slots[k].line = 0; }
			slots[k].used = false;
		}
	}
	inline void SlotKeep(DrawSlot* slots, int n) { for (int k = 0; k < n; ++k) if (slots[k].line != 0) slots[k].used = true; }

	inline int LevelBornIdx(const ChartState& S, int kind, int i)
	{
		const BaseState& B = S.base; const AuctionState& A = S.auction;
		switch (kind)
		{
		case LVL_POC: case LVL_VAH: case LVL_VAL: case LVL_OPEN: return (i < static_cast<int>(B.rthStartIdx.size()) && B.rthStartIdx[i] >= 0) ? B.rthStartIdx[i] : i;
		case LVL_IBH: case LVL_IBL: case LVL_IBEXT: return A.ibCloseIdx >= 0 ? A.ibCloseIdx : i;
		case LVL_PD_POC: case LVL_PD_VAH: case LVL_PD_VAL: case LVL_PDH: case LVL_PDL: case LVL_PDC: case LVL_ONH: case LVL_ONL: case LVL_PWH: case LVL_PWL: return (i < static_cast<int>(B.dayStartIdx.size())) ? B.dayStartIdx[i] : i;
		case LVL_SWING_H: return A.lastSwingHigh >= 0 ? A.swings[A.lastSwingHigh].idx : i;
		case LVL_SWING_L: return A.lastSwingLow >= 0 ? A.swings[A.lastSwingLow].idx : i;
		default: return Max(0, i - 60);
		}
	}

	// GDI: records the region's pixel size (the study sizes HUD rows and pill gaps from it), then draws the
	// docked profile (right edge of the fill space) and the calculated-values strip (bottom of the price region)
	void DrawTerminalGDI(HWND WindowHandle, HDC DeviceContext, SCStudyInterfaceRef sc)
	{
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		ChartState* Sp = Peek(sc.ChartNumber); if (!Sp || sc.ArraySize <= 0 || sc.Graphics.FillRectangle == nullptr) return;
		ChartState& S = *Sp; const VisualConfig& V = S.vis;
		render::Frame F(sc, S, sc.GraphRegion);
		S.term.regionH = Max(0, F.bottom - F.top); S.term.regionW = Max(0, F.right - F.left);
		if (!V.layer[L_PROFILE] && !V.layer[L_TAPE] && !V.layer[L_FOOTPRINT]) return;
		const double t0 = render_entry::NowMs();
		if (sc.Graphics.SetBackgroundMode) sc.Graphics.SetBackgroundMode(TRANSPARENT);
		if (sc.Graphics.SetTextAlign) sc.Graphics.SetTextAlign(TA_LEFT | TA_TOP | TA_NOUPDATECP);
		F.Clip(F.left, F.top, F.right, F.bottom);
		if (V.layer[L_FOOTPRINT]) render::DrawFootprint(F);
		if (V.layer[L_PROFILE]) render::DrawProfile(F);
		if (V.layer[L_TAPE])
		{
			render::Frame G(sc, S, sc.GraphRegion);
			G.top = G.bottom - Max(40, (G.bottom - G.top) * 11 / 100);
			render::DrawTape(G, V.tapeRows);
		}
		F.Unclip();
		S.perf.overlayMs = 0.8 * S.perf.overlayMs + 0.2 * (render_entry::NowMs() - t0);
	}
}

SCSFExport scsf_NQEdge_Terminal(SCStudyInterfaceRef sc)
{
	using namespace term;
	if (sc.SetDefaults)
	{
		sc.GraphName = "NQ Edge Terminal";
		sc.StudyDescription = "The whole NQ Edge system in one study, drawn only in the price region. PRO preset: bias candles, VWAP band, nearest-level pills, signal boxes with $-risk size, HUD, docked volume profile, swing-delta and per-bar delta numbers, regression channel, fib levels of the last leg, event log with markers, projection arrow, calculated-values strip. CLEAN keeps only the first five. Every layer is switchable.";
		sc.GraphRegion = 0; sc.AutoLoop = 0; sc.CalculationPrecedence = VERY_LOW_PREC_LEVEL; sc.ValueFormat = VALUEFORMAT_INHERITED; sc.ScaleRangeType = SCALE_SAMEASREGION; sc.DrawZeros = 0;
		sc.MaintainVolumeAtPriceData = 1;
		const char* names[TS_COUNT] = { "Bias Candle", "Forming Bar", "VWAP", "VWAP +1 SD", "VWAP -1 SD", "Long Signal", "Short Signal",
			"h.DCS", "h.DCS Smoothed", "h.Regime", "h.MTF Bias", "h.CVD", "h.Delta", "h.Structure", "h.Value Position", "h.POC", "h.VAH", "h.VAL", "h.Signal", "h.Grade" };
		for (int k = 0; k < TS_COUNT; ++k) { sc.Subgraph[k].Name = names[k]; sc.Subgraph[k].DrawZeros = 0; sc.Subgraph[k].DrawStyle = DRAWSTYLE_IGNORE; sc.Subgraph[k].LineWidth = 1; }
		sc.Subgraph[TS_CANDLE].DrawStyle = DRAWSTYLE_COLOR_BAR; sc.Subgraph[TS_CANDLE].PrimaryColor = RGB(138, 147, 166);
		sc.Subgraph[TS_CANDLE_FORMING].DrawStyle = DRAWSTYLE_COLOR_BAR_HOLLOW; sc.Subgraph[TS_CANDLE_FORMING].PrimaryColor = RGB(138, 147, 166);
		sc.Subgraph[TS_VWAP].DrawStyle = DRAWSTYLE_LINE; sc.Subgraph[TS_VWAP].LineWidth = 2; sc.Subgraph[TS_VWAP].PrimaryColor = RGB(255, 200, 87);
		sc.Subgraph[TS_BAND_TOP].DrawStyle = DRAWSTYLE_LINE; sc.Subgraph[TS_BAND_TOP].LineStyle = LINESTYLE_DOT; sc.Subgraph[TS_BAND_TOP].PrimaryColor = RGB(255, 200, 87);
		sc.Subgraph[TS_BAND_BOT].DrawStyle = DRAWSTYLE_LINE; sc.Subgraph[TS_BAND_BOT].LineStyle = LINESTYLE_DOT; sc.Subgraph[TS_BAND_BOT].PrimaryColor = RGB(255, 200, 87);
		sc.Subgraph[TS_ARROW_UP].DrawStyle = DRAWSTYLE_ARROW_UP; sc.Subgraph[TS_ARROW_UP].LineWidth = 3; sc.Subgraph[TS_ARROW_UP].PrimaryColor = RGB(0, 200, 150);
		sc.Subgraph[TS_ARROW_DN].DrawStyle = DRAWSTYLE_ARROW_DOWN; sc.Subgraph[TS_ARROW_DN].LineWidth = 3; sc.Subgraph[TS_ARROW_DN].PrimaryColor = RGB(255, 77, 94);

		sc.Input[TI_PRESET].Name = "Preset"; sc.Input[TI_PRESET].SetCustomInputStrings("CLEAN;PRO"); sc.Input[TI_PRESET].SetCustomInputIndex(1);
		sc.Input[TI_RTH_START].Name = "Session: RTH Start"; sc.Input[TI_RTH_START].SetTime(HMS_TIME(9, 30, 0));
		sc.Input[TI_RTH_END].Name = "Session: RTH End"; sc.Input[TI_RTH_END].SetTime(HMS_TIME(16, 0, 0));
		sc.Input[TI_DAY_START].Name = "Session: Trading Day Start (evening open)"; sc.Input[TI_DAY_START].SetTime(HMS_TIME(18, 0, 0));
		NQE_INT_INPUT(TI_ATR_LEN, "Session: ATR Length", 14, 2, 500);
		NQE_FLT_INPUT(TI_VA_PCT, "Profile: Value Area %", 70.0, 50.0, 95.0);
		NQE_INT_INPUT(TI_IB_MIN, "Profile: Initial Balance Minutes", 60, 5, 240);
		NQE_INT_INPUT(TI_SWING_N, "Structure: Swing Strength Bars", 5, 2, 50);
		NQE_FLT_INPUT(TI_SWING_ATR, "Structure: Swing Min Distance (ATR)", 0.5, 0.0, 10.0);
		NQE_FLT_INPUT(TI_IMB_RATIO, "Flow: Imbalance Ratio %", 300.0, 150.0, 2000.0);
		NQE_INT_INPUT(TI_IMB_STACK, "Flow: Stacked Levels >=", 3, 2, 20);
		NQE_FLT_INPUT(TI_ABS_Z, "Flow: Absorption Volume Z >=", 2.0, 0.5, 10.0);
		sc.Input[TI_C_YM].Name = "Chart Number: YM (0 = off)"; sc.Input[TI_C_YM].SetChartNumber(0);
		sc.Input[TI_C_ES].Name = "Chart Number: ES (0 = off)"; sc.Input[TI_C_ES].SetChartNumber(0);
		sc.Input[TI_C_TICK].Name = "Chart Number: NYSE TICK (0 = off)"; sc.Input[TI_C_TICK].SetChartNumber(0);
		sc.Input[TI_C_MEGA1].Name = "Chart Number: Mega Cap 1 (AAPL)"; sc.Input[TI_C_MEGA1].SetChartNumber(0);
		sc.Input[TI_C_MEGA2].Name = "Chart Number: Mega Cap 2 (AMZN)"; sc.Input[TI_C_MEGA2].SetChartNumber(0);
		sc.Input[TI_C_MEGA3].Name = "Chart Number: Mega Cap 3 (NVDA)"; sc.Input[TI_C_MEGA3].SetChartNumber(0);
		sc.Input[TI_WEIGHTS].Name = "Model: Weights File (Data folder)"; sc.Input[TI_WEIGHTS].SetString("NQEdge_weights.txt");
		NQE_FLT_INPUT(TI_THR, "Model: Signal Threshold |DCS| (0 = from file)", 0.0, 0.0, 100.0);
		NQE_FLT_INPUT(TI_STRONG, "Model: Strong Bias |DCS| >=", 60.0, 0.0, 100.0);
		NQE_FLT_INPUT(TI_WEAK, "Model: Candle Bias |DCS| >=", 25.0, 0.0, 100.0);
		NQE_YESNO_INPUT(TI_S1, "Setup: Trend Pullback", 1);
		NQE_YESNO_INPUT(TI_S2, "Setup: Value-Edge Rejection", 1);
		NQE_YESNO_INPUT(TI_S3, "Setup: Failed Breakout / Trapped", 1);
		NQE_YESNO_INPUT(TI_S4, "Setup: Break-and-Acceptance", 1);
		NQE_YESNO_INPUT(TI_S5, "Setup: Divergence At Liquidity", 1);
		sc.Input[TI_MIN_GRADE].Name = "Signals Shown: Minimum Grade"; sc.Input[TI_MIN_GRADE].SetCustomInputStrings("A only;A and B;All"); sc.Input[TI_MIN_GRADE].SetCustomInputIndex(1);
		NQE_YESNO_INPUT(TI_ALERTS, "Alerts Enabled", 1);
		sc.Input[TI_SOUND].Name = "Alert Sound Number"; sc.Input[TI_SOUND].SetAlertSoundNumber(1);
		NQE_YESNO_INPUT(TI_LOG, "Feature Logging (CSV in Data folder)", 1);
		sc.Input[TI_VWAP_ANCHOR].Name = "VWAP Anchor"; sc.Input[TI_VWAP_ANCHOR].SetCustomInputStrings("Trading Day Start (18:00);RTH Open"); sc.Input[TI_VWAP_ANCHOR].SetCustomInputIndex(0);
		NQE_FLT_INPUT(TI_RISK, "Risk Per Trade ($, size suggestion)", 100.0, 0.0, 1000000.0);
		sc.Input[TI_ETH].Name = "Signals Outside RTH"; sc.Input[TI_ETH].SetCustomInputStrings("Off;Grade A only;All"); sc.Input[TI_ETH].SetCustomInputIndex(1);
		NQE_FLT_INPUT(TI_MIN_STOP, "Setup: Min Stop (ATR)", 0.6, 0.1, 3.0);
		NQE_FLT_INPUT(TI_MAX_STOP, "Setup: Max Stop (ATR)", 2.5, 0.5, 10.0);
		NQE_FLT_INPUT(TI_MIN_RR, "Setup: Min R:R To T1", 1.0, 0.5, 5.0);
		NQE_FLT_INPUT(TI_DAY_LOSS, "Risk: Daily Loss Limit ($, 0 = off)", 500.0, 0.0, 1000000.0);
		NQE_INT_INPUT(TI_MAX_TRADES, "Risk: Max Trades Per Day (0 = off)", 6, 0, 100);
		sc.Input[TI_TRADE_START].Name = "Live: Trade Window Start (end <= start = always)"; sc.Input[TI_TRADE_START].SetTime(HMS_TIME(10, 0, 0));
		sc.Input[TI_TRADE_END].Name = "Live: Trade Window End"; sc.Input[TI_TRADE_END].SetTime(HMS_TIME(15, 30, 0));
		for (int k = 0; k < TL_COUNT; ++k)
		{
			SCString nm; nm.Format("Layer: %s", kTermLayerNames[k]);
			sc.Input[TI_LAYER0 + k].Name = nm; sc.Input[TI_LAYER0 + k].SetCustomInputStrings("Preset default;On;Off"); sc.Input[TI_LAYER0 + k].SetCustomInputIndex(0);
		}
		NQE_INT_INPUT(TI_FONT, "HUD Font Size (pt)", 9, 7, 20);
		NQE_INT_INPUT(TI_NUM_BARS, "Delta Per Bar: Last N Bars", 30, 5, 60);
		NQE_INT_INPUT(TI_PROFILE_W, "Profile Width (% of fill space)", 25, 10, 60);
		sc.Input[TI_BAND_STYLE].Name = "VWAP Band Style"; sc.Input[TI_BAND_STYLE].SetCustomInputStrings("Dotted lines;Filled;Off"); sc.Input[TI_BAND_STYLE].SetCustomInputIndex(0);
		NQE_INT_INPUT(TI_BAND_ALPHA, "VWAP Band Fill Transparency %", 90, 50, 98);
		NQE_INT_INPUT(TI_TL_SWINGS, "Trend Lines: Swings Scanned", 8, 2, 30);
		NQE_COLOR_INPUT(TI_COL_BULL, "Color: Bull", 0, 200, 150);
		NQE_COLOR_INPUT(TI_COL_BEAR, "Color: Bear", 255, 77, 94);
		NQE_COLOR_INPUT(TI_COL_NEUTRAL, "Color: Neutral (down bar)", 110, 118, 134);
		NQE_COLOR_INPUT(TI_COL_NEUTRAL_UP, "Color: Neutral (up bar)", 158, 166, 184);
		NQE_COLOR_INPUT(TI_COL_VWAP, "Color: VWAP", 255, 200, 87);
		NQE_COLOR_INPUT(TI_COL_LEVEL, "Color: Levels", 62, 198, 255);
		NQE_COLOR_INPUT(TI_COL_LONG, "Color: Long Signal", 0, 200, 150);
		NQE_COLOR_INPUT(TI_COL_SHORT, "Color: Short Signal", 255, 77, 94);
		NQE_COLOR_INPUT(TI_COL_TEXT, "Color: HUD Text", 230, 234, 242);
		NQE_COLOR_INPUT(TI_COL_PANEL, "Color: Pill Text (dark)", 11, 14, 20);
		NQE_COLOR_INPUT(TI_COL_DIM, "Color: Dim Text", 120, 128, 145);
		return;
	}
	sc.p_GDIFunction = DrawTerminalGDI;
	if (sc.LastCallToFunction)
	{
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		ChartState* Sp = Peek(sc.ChartNumber);
		if (Sp) { TermState& T = Sp->term; SlotFlush(sc, T.all, TermState::SLOTS, true); Sp->terminalPresent = false; }
		Release(sc, -1); return;
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	ChartState& S = Acquire(sc);
	S.terminalPresent = true;
	for (int e = E_AUCTION; e <= E_LOG; ++e) S.studyPresent[e] = true;
	++S.hudUpdates;
	if (sc.MaintainVolumeAtPriceData == 0) { sc.MaintainVolumeAtPriceData = 1; sc.FlagToReloadChartData = 1; }

	// ---- parameters: exposed inputs override the engine defaults ----
	{
		BaseParams bp{}; bp.rthStartSec = sc.Input[TI_RTH_START].GetTime(); bp.rthEndSec = sc.Input[TI_RTH_END].GetTime(); bp.dayStartSec = sc.Input[TI_DAY_START].GetTime(); bp.atrLength = sc.Input[TI_ATR_LEN].GetInt(); SetParams(S, E_BASE, S.params.base, bp);
		AuctionParams ap{}; ap.valueAreaPct = sc.Input[TI_VA_PCT].GetFloat(); ap.ibMinutes = sc.Input[TI_IB_MIN].GetInt(); ap.swingStrength = sc.Input[TI_SWING_N].GetInt(); ap.swingMinAtr = sc.Input[TI_SWING_ATR].GetFloat(); SetParams(S, E_AUCTION, S.params.auction, ap);
		VwapParams vp{}; vp.anchor = sc.Input[TI_VWAP_ANCHOR].GetIndex() == 0 ? 1 : 0; SetParams(S, E_VWAP, S.params.vwap, vp);
		FlowParams fp{}; fp.imbRatioPct = sc.Input[TI_IMB_RATIO].GetFloat(); fp.imbStackLevels = sc.Input[TI_IMB_STACK].GetInt(); fp.absorbVolZ = sc.Input[TI_ABS_Z].GetFloat(); SetParams(S, E_FLOW, S.params.flow, fp);
		RegimeParams rp{}; SetParams(S, E_REGIME, S.params.regime, rp);
		InterParams ip{}; ip.chartYM = sc.Input[TI_C_YM].GetChartNumber(); ip.chartES = sc.Input[TI_C_ES].GetChartNumber(); ip.chartTICK = sc.Input[TI_C_TICK].GetChartNumber();
		ip.chartMega[0] = sc.Input[TI_C_MEGA1].GetChartNumber(); ip.chartMega[1] = sc.Input[TI_C_MEGA2].GetChartNumber(); ip.chartMega[2] = sc.Input[TI_C_MEGA3].GetChartNumber(); SetParams(S, E_INTER, S.params.inter, ip);
		DcsParams dp{}; strncpy_s(dp.weightsFile, sizeof(dp.weightsFile), sc.Input[TI_WEIGHTS].GetString(), _TRUNCATE); dp.signalThr = sc.Input[TI_THR].GetFloat(); dp.strongThr = sc.Input[TI_STRONG].GetFloat(); dp.weakThr = sc.Input[TI_WEAK].GetFloat();
		for (int k = 1; k < SETUP_COUNT; ++k) dp.setupOn[k] = sc.Input[TI_S1 + k - 1].GetYesNo();
		dp.alertsOn = sc.Input[TI_ALERTS].GetYesNo(); dp.alertSound = sc.Input[TI_SOUND].GetAlertSoundNumber(); dp.alertMinGrade = sc.Input[TI_MIN_GRADE].GetIndex() + 1;
		dp.riskPerTrade = sc.Input[TI_RISK].GetFloat(); dp.ethSignals = sc.Input[TI_ETH].GetIndex(); dp.minStopAtr = sc.Input[TI_MIN_STOP].GetFloat(); dp.maxStopAtr = sc.Input[TI_MAX_STOP].GetFloat(); dp.minRR = sc.Input[TI_MIN_RR].GetFloat();
		dp.tradeStartSec = sc.Input[TI_TRADE_START].GetTime(); dp.tradeEndSec = sc.Input[TI_TRADE_END].GetTime();
		SetParams(S, E_DCS, S.params.dcs, dp);
		ValParams vlp{}; SetParams(S, E_VAL, S.params.val, vlp);
		LogParams lp{}; lp.enabled = sc.Input[TI_LOG].GetYesNo(); SetParams(S, E_LOG, S.params.log, lp);
	}
	// ---- layers: preset defaults with per-layer overrides ----
	const int preset = Clamp(static_cast<int>(sc.Input[TI_PRESET].GetIndex()), 0, 1);
	const bool pro = preset == 1;
	bool on[TL_COUNT];
	for (int k = 0; k < TL_COUNT; ++k) { const int tri = sc.Input[TI_LAYER0 + k].GetIndex(); on[k] = tri == 1 ? true : (tri == 2 ? false : kTermPreset[preset][k] != 0); }
	VisualConfig& V = S.vis;
	V.preset = pro ? PRESET_COMMAND : PRESET_CLEAN; V.diagnostics = false; V.fontPt = sc.Input[TI_FONT].GetInt(); V.minGrade = sc.Input[TI_MIN_GRADE].GetIndex() + 1; V.tapeRows = 63; V.maxNotes = 6;
	for (int k = 0; k < L_COUNT; ++k) V.layer[k] = false;
	V.layer[L_CANDLES] = on[TL_CANDLES]; V.layer[L_LEVELS] = on[TL_LEVELS]; V.layer[L_CARDS] = on[TL_SIGNALS]; V.layer[L_HUD] = on[TL_HUD];
	V.layer[L_PROFILE] = on[TL_PROFILE]; V.layer[L_GHOST] = on[TL_PROFILE]; V.layer[L_ZONES] = on[TL_ZONES]; V.layer[L_BUBBLES] = on[TL_BUBBLES];
	V.layer[L_SWINGS] = on[TL_SWING]; V.layer[L_NOTES] = on[TL_NOTES]; V.layer[L_PROJECTION] = on[TL_PROJ]; V.layer[L_TAPE] = on[TL_TAPE]; V.layer[L_CHANNEL] = on[TL_CHANNEL]; V.layer[L_FOOTPRINT] = on[TL_FOOTPRINT];
	V.profileWidthPct = sc.Input[TI_PROFILE_W].GetInt(); V.profileDockRight = true;
	Theme& TH = V.theme;
	TH.bull = sc.Input[TI_COL_BULL].GetColor(); TH.bear = sc.Input[TI_COL_BEAR].GetColor(); TH.neutral = sc.Input[TI_COL_NEUTRAL].GetColor(); TH.gold = sc.Input[TI_COL_VWAP].GetColor(); TH.cyan = sc.Input[TI_COL_LEVEL].GetColor();
	TH.text = sc.Input[TI_COL_TEXT].GetColor(); TH.panel = sc.Input[TI_COL_PANEL].GetColor(); TH.bg = sc.Input[TI_COL_PANEL].GetColor(); TH.dim = sc.Input[TI_COL_DIM].GetColor();

	if (sc.IsFullRecalculation && sc.UpdateStartIndex == 0) ResetFrom(S, E_BASE);   // reload / back-fill / setting change: rebuild every engine from bar 0
	CheckDataStamp(sc, S);
	CheckWarnings(sc, S);
	{
		LARGE_INTEGER f0, t0, t1; QueryPerformanceFrequency(&f0); QueryPerformanceCounter(&t0);
		EnsureLog(sc, S);
		QueryPerformanceCounter(&t1);
		const double ms = f0.QuadPart > 0 ? 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f0.QuadPart) : 0.0;
		S.hud.updateMs = ms; if (sc.UpdateStartIndex > 0) S.hud.maxUpdateMs = Max(S.hud.maxUpdateMs, ms); S.hud.fullCalcMs = sc.LastFullCalculationTimeInMicroseconds / 1000; S.hud.bars = sc.ArraySize;
	}
	BuildHudSnapshot(sc, S);
	const int n = sc.ArraySize; if (n <= 0) return;
	const int lastClosed = n - 2;
	const HudSnapshot& H = S.hud; const DcsState& D = S.dcs; const VwapState& W = S.vwap; const AuctionState& A = S.auction; const FlowState& FL = S.flow;
	const float atr = AtrAt(S, Max(0, lastClosed)); const float close = sc.Close[n - 1]; const float tick = S.tickSize;
	const uint32_t cNeuUp = sc.Input[TI_COL_NEUTRAL_UP].GetColor();
	const uint32_t cBull = TH.bull, cBear = TH.bear, cNeu = TH.neutral, cVwap = TH.gold, cLevel = TH.cyan, cLong = sc.Input[TI_COL_LONG].GetColor(), cShort = sc.Input[TI_COL_SHORT].GetColor(), cText = TH.text, cPanel = TH.panel, cDim = TH.dim;
	const int fontPt = V.fontPt;
	char buf[256];
	#define PXS(v) sc.FormatGraphValue((v), sc.BaseGraphValueFormat).GetChars()

	// ---- subgraphs: candles, VWAP + band, arrows, hidden helpers ----
	const bool candles = on[TL_CANDLES], band = on[TL_BAND], signals = on[TL_SIGNALS];
	const int bandStyle = (!band) ? 2 : Clamp(static_cast<int>(sc.Input[TI_BAND_STYLE].GetIndex()), 0, 2);   // 0 dotted lines, 1 fill, 2 off
	sc.Subgraph[TS_CANDLE].DrawStyle = candles ? DRAWSTYLE_COLOR_BAR : DRAWSTYLE_IGNORE; sc.Subgraph[TS_CANDLE_FORMING].DrawStyle = candles ? DRAWSTYLE_COLOR_BAR_HOLLOW : DRAWSTYLE_IGNORE;
	sc.Subgraph[TS_VWAP].DrawStyle = band ? DRAWSTYLE_LINE : DRAWSTYLE_IGNORE; sc.Subgraph[TS_VWAP].PrimaryColor = cVwap;
	sc.Subgraph[TS_BAND_TOP].DrawStyle = bandStyle == 2 ? DRAWSTYLE_IGNORE : (bandStyle == 1 ? DRAWSTYLE_TRANSPARENT_FILL_TOP : DRAWSTYLE_LINE);
	sc.Subgraph[TS_BAND_BOT].DrawStyle = bandStyle == 2 ? DRAWSTYLE_IGNORE : (bandStyle == 1 ? DRAWSTYLE_TRANSPARENT_FILL_BOTTOM : DRAWSTYLE_LINE);
	sc.Subgraph[TS_BAND_TOP].LineStyle = sc.Subgraph[TS_BAND_BOT].LineStyle = LINESTYLE_DOT; sc.Subgraph[TS_BAND_TOP].LineWidth = sc.Subgraph[TS_BAND_BOT].LineWidth = 1;
	sc.Subgraph[TS_BAND_TOP].PrimaryColor = sc.Subgraph[TS_BAND_BOT].PrimaryColor = cVwap;
	sc.Subgraph[TS_ARROW_UP].DrawStyle = signals ? DRAWSTYLE_ARROW_UP : DRAWSTYLE_IGNORE; sc.Subgraph[TS_ARROW_DN].DrawStyle = signals ? DRAWSTYLE_ARROW_DOWN : DRAWSTYLE_IGNORE;
	sc.Subgraph[TS_ARROW_UP].PrimaryColor = cLong; sc.Subgraph[TS_ARROW_DN].PrimaryColor = cShort;
	int& bandApplied = sc.GetPersistentInt(4);
	const int bandAlpha = sc.Input[TI_BAND_ALPHA].GetInt();
	if (bandApplied != bandAlpha && sc.SetChartStudyTransparencyLevel != nullptr) { sc.SetChartStudyTransparencyLevel(sc.ChartNumber, sc.StudyGraphInstanceID, bandAlpha); bandApplied = bandAlpha; }
	int& gen = sc.GetPersistentInt(1);
	int start = Min(sc.UpdateStartIndex, Min(S.dcs.dirtyFrom, S.vwap.dirtyFrom)); S.dcs.dirtyFrom = INT_MAX; S.vwap.dirtyFrom = INT_MAX;
	if (gen != S.dcs.generation) { start = 0; gen = S.dcs.generation; }
	if (start < 0) start = 0;
	const float weak = S.params.dcs.weakThr;
	const int minGrade = V.minGrade;
	for (int i = Max(0, start - 1); i < n; ++i)
	{
		const float v = D.dcs[i];
		const uint32_t cc = v >= weak ? cBull : (v <= -weak ? cBear : (sc.Close[i] >= sc.Open[i] ? cNeuUp : cNeu));
		const bool forming = (i == n - 1);
		sc.Subgraph[TS_CANDLE][i] = (candles && !forming) ? 1.0f : 0.0f; sc.Subgraph[TS_CANDLE].DataColor[i] = cc;
		sc.Subgraph[TS_CANDLE_FORMING][i] = (candles && forming) ? 1.0f : 0.0f; sc.Subgraph[TS_CANDLE_FORMING].DataColor[i] = cc;
		sc.Subgraph[TS_VWAP][i] = W.vwap[i]; sc.Subgraph[TS_BAND_TOP][i] = W.vwap[i] > 0 ? W.b1u[i] : 0.0f; sc.Subgraph[TS_BAND_BOT][i] = W.vwap[i] > 0 ? W.b1d[i] : 0.0f;
		sc.Subgraph[TS_ARROW_UP][i] = 0; sc.Subgraph[TS_ARROW_DN][i] = 0;
		sc.Subgraph[TS_H_DCS][i] = v; sc.Subgraph[TS_H_DCS_SMOOTH][i] = D.dcsSmooth[i]; sc.Subgraph[TS_H_REGIME][i] = S.regime.regime[i]; sc.Subgraph[TS_H_MTF][i] = S.regime.mtfBias[i];
		sc.Subgraph[TS_H_CVD][i] = FL.cvd[i]; sc.Subgraph[TS_H_DELTA][i] = FL.delta[i]; sc.Subgraph[TS_H_STRUCT][i] = A.structTrend[i]; sc.Subgraph[TS_H_VAPOS][i] = A.vaPos[i];
		sc.Subgraph[TS_H_POC][i] = A.poc[i]; sc.Subgraph[TS_H_VAH][i] = A.vah[i]; sc.Subgraph[TS_H_VAL][i] = A.val[i];
		sc.Subgraph[TS_H_SIGNAL][i] = static_cast<float>(D.signalType[i]) * static_cast<float>(D.signalDir[i]); sc.Subgraph[TS_H_GRADE][i] = 0;
	}
	for (int k = static_cast<int>(D.signals.size()) - 1; k >= 0 && D.signals[k].idx >= start - 1; --k)
	{
		const Signal& g = D.signals[k]; if (g.idx < 0 || g.idx >= n) continue;
		sc.Subgraph[TS_H_GRADE][g.idx] = static_cast<float>(g.grade);
		if (g.grade > minGrade) continue;
		if (g.dir > 0) sc.Subgraph[TS_ARROW_UP][g.idx] = sc.Low[g.idx] - 0.25f * AtrAt(S, g.idx); else sc.Subgraph[TS_ARROW_DN][g.idx] = sc.High[g.idx] + 0.25f * AtrAt(S, g.idx);
	}

	// ---- managed drawings (delete + redraw by line number every update) ----
	TermState& T = S.term;
	const int fillBars = static_cast<int>(sc.NumFillSpaceBars);
	// pixel geometry: bar spacing and the region height (from the GDI pass) size the pills and the HUD rows
	const int barPx = sc.ChartBarSpacing > 0 ? static_cast<int>(sc.ChartBarSpacing) : 8;
	const float charPx = 0.62f * 1.33f * static_cast<float>(fontPt);
	// fill-space budget: HUD text, pills and the docked profile sit side by side. Shrink the HUD font one point, then the
	// profile (down to 12 %), and tell the user how many fill-space bars the layout needs.
	int hudPt = fontPt; int profPct = on[TL_PROFILE] ? Clamp(sc.Input[TI_PROFILE_W].GetInt(), 10, 60) : 0; int fillNeeded = 0;
	{
		const float fillPx = static_cast<float>(Max(1, fillBars) * barPx);
		const float pillPx = 13.0f * charPx + 2.0f * barPx;
		float hudPx = on[TL_HUD] ? 48.0f * 0.62f * 1.33f * static_cast<float>(hudPt) + 2.0f * barPx : 0.0f;
		float profPx = fillPx * static_cast<float>(profPct) / 100.0f;
		if (on[TL_HUD] && hudPx + pillPx + profPx > fillPx && hudPt > 8) { hudPt = Max(8, fontPt - 1); hudPx = 48.0f * 0.62f * 1.33f * static_cast<float>(hudPt) + 2.0f * barPx; }
		if (on[TL_PROFILE] && hudPx + pillPx + profPx > fillPx) { profPct = Max(12, static_cast<int>(100.0f * Max(0.0f, fillPx - hudPx - pillPx) / fillPx)); profPx = fillPx * static_cast<float>(profPct) / 100.0f; }
		fillNeeded = static_cast<int>((hudPx + pillPx + profPx) / static_cast<float>(barPx)) + 1;
	}
	if (on[TL_PROFILE]) V.profileWidthPct = profPct;
	const int profBars = on[TL_PROFILE] ? Max(4, fillBars * profPct / 100) : 0;
	const int pillEnd = n - 1 + Max(4, fillBars - profBars - 1);                                          // pills are right-aligned here (left of the profile)
	const int pillCol1 = pillEnd - Max(6, static_cast<int>(charPx * 13.0f / barPx) + 1);                  // second column when two pills would overlap
	const int lineEnd = Max(n, pillCol1 - 2);
	double visHi = 0, visLo = 0; if (sc.GetGraphVisibleHighAndLow != nullptr) sc.GetGraphVisibleHighAndLow(visHi, visLo);
	const double visRange = visHi > visLo ? visHi - visLo : 0.0;
	const float pillH = visRange > 0 ? static_cast<float>(T.regionH > 0 ? (1.33 * 1.5 * fontPt) * visRange / T.regionH : 0.024 * visRange) : 0.0f;   // pill height in price units
	const float pitch = T.regionH > 0 ? Clamp(100.0f * (1.33f * 1.35f * static_cast<float>(hudPt)) / static_cast<float>(T.regionH), 1.2f, 4.0f) : 3.2f;   // HUD row pitch in % of the region
	const double relX = -2.0;                                                                              // HUD / log anchor: 2 bars right of the last bar
	// heavy per-bar layers are redrawn when a bar closes, the layer set changes or the engines were rebuilt
	int& lastN = sc.GetPersistentInt(2); int& lastMask = sc.GetPersistentInt(3);
	int mask = preset << 20; for (int k = 0; k < TL_COUNT; ++k) if (on[k]) mask |= 1 << k;
	const bool heavy = lastN != n || lastMask != mask || sc.IsFullRecalculation != 0 || start == 0;
	lastN = n; lastMask = mask;
	// ---- position + daily risk guard from Sierra's trade position (live or Trade Simulation Mode) ----
	s_SCPositionData pos; const bool havePos = sc.GetTradePosition(pos) != 0;
	const float dayLoss = sc.Input[TI_DAY_LOSS].GetFloat(); const int maxTrades = sc.Input[TI_MAX_TRADES].GetInt();
	const double dayPnl = havePos ? pos.DailyProfitLoss + pos.OpenProfitLoss : 0.0;
	const bool limitHit = havePos && ((dayLoss > 0 && dayPnl <= -dayLoss) || (maxTrades > 0 && pos.TotalTrades >= maxTrades));
	std::vector<dcs_detail::Lv> lv; dcs_detail::GatherLevels(sc, S, Max(0, lastClosed), lv);
	std::vector<float> pillPrices;

	// nearest levels: 2+2 (CLEAN) or 3+3 (PRO), equal prices merged, VWAP bands and zones excluded; right-aligned pills in two columns
	if (on[TL_LEVELS] && lastClosed >= 0)
	{
		struct L4 { float price; int kind; char text[40]; };
		std::vector<L4> cand; cand.reserve(lv.size());
		for (size_t k = 0; k < lv.size(); ++k)
		{
			const int kind = lv[k].kind;
			if (kind == LVL_VWAP || kind == LVL_VWAP_B1U || kind == LVL_VWAP_B1D || kind == LVL_VWAP_B2U || kind == LVL_VWAP_B2D || kind == LVL_VWAP_B3U || kind == LVL_VWAP_B3D) continue;
			if (kind == LVL_ABSORB || kind == LVL_IMB || kind == LVL_SINGLE_PRINT || kind == LVL_FAILED) continue;
			bool merged = false;
			for (size_t q = 0; q < cand.size(); ++q) if (fabs(cand[q].price - lv[k].price) < 0.5f * tick) { if (strlen(cand[q].text) < 24) { strcat_s(cand[q].text, sizeof(cand[q].text), "/"); strcat_s(cand[q].text, sizeof(cand[q].text), dcs_detail::LevelName(kind)); } merged = true; break; }
			if (merged) continue;
			L4 c; c.price = lv[k].price; c.kind = kind; strcpy_s(c.text, sizeof(c.text), dcs_detail::LevelName(kind)); cand.push_back(c);
		}
		std::sort(cand.begin(), cand.end(), [](const L4& a, const L4& b) { return a.price < b.price; });
		const int per = pro ? 3 : 2;
		int pick[TermState::LVL_SLOTS]; int np = 0;
		int above = -1, below = -1;
		for (size_t q = 0; q < cand.size(); ++q) { if (cand[q].price > close && above < 0) above = static_cast<int>(q); if (cand[q].price <= close) below = static_cast<int>(q); }
		for (int k = 0; k < per && above >= 0 && above + k < static_cast<int>(cand.size()) && np < TermState::LVL_SLOTS; ++k) pick[np++] = above + k;
		for (int k = 0; k < per && below >= 0 && below - k >= 0 && np < TermState::LVL_SLOTS; ++k) pick[np++] = below - k;
		std::sort(pick, pick + np);   // ascending price, so a pill that would sit on the previous one moves one column left
		const float tol = S.params.dcs.levelTolAtr * atr;
		float lastCol0 = -FLT_MAX;
		for (int k = 0; k < TermState::LVL_SLOTS; ++k)
		{
			if (k >= np) continue;
			const L4& c = cand[pick[k]];
			const bool isNear = fabs(c.price - close) <= tol;
			const int born = Max(0, LevelBornIdx(S, c.kind, lastClosed));
			sprintf_s(buf, sizeof(buf), " %s %s ", c.text, PXS(c.price));
			const int w = Max(4, static_cast<int>(charPx * static_cast<float>(strlen(buf)) / barPx) + 1);
			const bool col1 = (c.price - lastCol0) < pillH;
			if (!col1) lastCol0 = c.price;
			const int anchor = col1 ? pillCol1 : pillEnd;
			SlotLine(sc, T.lvlLine[k], born, Max(n, anchor - w), c.price, c.price, cLevel, 1, isNear ? LINESTYLE_SOLID : LINESTYLE_DOT);
			SlotText(sc, T.lvlTag[k], anchor, 0, c.price, false, buf, cPanel, Max(7, fontPt - 1), isNear, isNear ? cText : cLevel, true, DT_RIGHT | DT_VCENTER);
			pillPrices.push_back(c.price);
		}
	}

	// HUD geometry: candidate block tops = region top, lowest allowed, just below / above every pill (level pills and fib tags);
	// the candidate covering the fewest pills wins, ties prefer the ends and then the half away from price
	const int nl = pro ? 15 : 8;
	const float pillPitch = pitch * 1.7f;
	const float hudTotal = pillPitch + pitch * static_cast<float>(nl - 1);
	const float hudBase = on[TL_TAPE] ? 14.0f : 3.0f;
	const float priceY = visRange > 0 ? static_cast<float>(100.0 * (close - visLo) / visRange) : 50.0f;
	std::vector<float> pillY;
	if (visRange > 0) for (size_t q = 0; q < pillPrices.size(); ++q) pillY.push_back(static_cast<float>(100.0 * (pillPrices[q] - visLo) / visRange));
	if (visRange > 0 && on[TL_FIB] && !FL.legs.empty())
	{
		const FlowState::Leg& L2 = FL.legs.back(); static const float rr[4] = { 0.382f, 0.5f, 0.618f, 0.786f };
		for (int k = 0; k < 4; ++k) pillY.push_back(static_cast<float>(100.0 * (L2.toPrice - (L2.toPrice - L2.fromPrice) * rr[k] - visLo) / visRange));
	}
	const float halfPill = visRange > 0 ? static_cast<float>(50.0 * pillH / visRange) + 0.6f : 1.5f;
	const float lowest = Min(97.0f, hudBase + hudTotal);
	float hudY0 = 97.0f;
	if (on[TL_HUD])
	{
		std::vector<float> cands; cands.push_back(97.0f); cands.push_back(lowest);
		for (size_t q = 0; q < pillY.size(); ++q) { cands.push_back(Clamp(pillY[q] - halfPill - 0.4f, lowest, 97.0f)); cands.push_back(Clamp(pillY[q] + halfPill + hudTotal + 0.4f, lowest, 97.0f)); }
		int bestScore = INT_MAX;
		for (size_t c = 0; c < cands.size(); ++c)
		{
			const float top = cands[c], bot = top - hudTotal; int hits = 0;
			for (size_t q = 0; q < pillY.size(); ++q) if (pillY[q] + halfPill >= bot && pillY[q] - halfPill <= top) ++hits;
			const int atEnd = (top >= 96.9f || top <= lowest + 0.1f) ? 0 : 1;
			const int nearPrice = ((0.5f * (top + bot) > 50.0f) == (priceY > 50.0f)) ? 1 : 0;
			const int score = hits * 100 + atEnd * 10 + nearPrice;
			if (score < bestScore) { bestScore = score; hudY0 = top; }
		}
	}
	const bool hudTop = (hudY0 - 0.5f * hudTotal) > 50.0f;
	const float hudPLo = visRange > 0 ? static_cast<float>(visLo + visRange * (hudY0 - hudTotal) / 100.0) : 0.0f, hudPHi = visRange > 0 ? static_cast<float>(visLo + visRange * hudY0 / 100.0) : 0.0f;

	// fib retracements of the last completed leg; a tag is skipped where a level pill or the HUD already sits
	if (on[TL_FIB] && !FL.legs.empty())
	{
		const FlowState::Leg& L2 = FL.legs.back();
		static const float r[TermState::FIB_SLOTS] = { 0.382f, 0.5f, 0.618f, 0.786f };
		for (int k = 0; k < TermState::FIB_SLOTS; ++k)
		{
			const float price = L2.toPrice - (L2.toPrice - L2.fromPrice) * r[k];
			SlotLine(sc, T.fibLine[k], L2.toIdx, lineEnd, price, price, cVwap, 1, LINESTYLE_DOT);
			bool clash = on[TL_HUD] && visRange > 0 && price >= hudPLo - pillH && price <= hudPHi + pillH;
			for (size_t q = 0; q < pillPrices.size() && !clash; ++q) if (fabs(pillPrices[q] - price) < Max(pillH, 0.3f * atr)) clash = true;
			if (clash) continue;
			sprintf_s(buf, sizeof(buf), " %.1f%% %s ", r[k] * 100, PXS(price));
			SlotText(sc, T.fibTag[k], pillEnd, 0, price, false, buf, cPanel, Max(7, fontPt - 2), false, cVwap, true, DT_RIGHT | DT_VCENTER);
			pillPrices.push_back(price);
		}
	}

	// signal boxes: the last 3 A/B signals that are live or within 2 ATR of price
	if (signals)
	{
		int slot = 0;
		for (int k = static_cast<int>(D.signals.size()) - 1; k >= 0 && slot < TermState::SIG_SLOTS; --k)
		{
			const Signal& g = D.signals[k];
			if (g.grade > minGrade) continue;
			const bool live = g.resolved == 0;
			if (!live && fabs(g.entry - close) > 2.0f * atr) continue;
			if (!live && lastClosed - g.resIdx > 60) continue;
			const int endIdx = live ? lineEnd : g.resIdx;
			TermState::SigDraw& sd = T.sig[slot++];
			SlotRect(sc, sd.risk, g.idx, endIdx, Max(g.entry, g.stop), Min(g.entry, g.stop), cShort, 80, nullptr);
			SlotRect(sc, sd.reward, g.idx, endIdx, Max(g.entry, g.t2), Min(g.entry, g.t2), cLong, 85, nullptr);
			SlotLine(sc, sd.t1, g.idx, endIdx, g.t1, g.t1, cLong, 1, LINESTYLE_DOT);
			const char grade = g.grade == 1 ? 'A' : (g.grade == 2 ? 'B' : 'C');
			char sz[16] = ""; if (g.size > 0) sprintf_s(sz, sizeof(sz), " | %dx", g.size);
			if (live) sprintf_s(buf, sizeof(buf), "%s %c | %s | R:R %.1f | stop %s%s", g.dir > 0 ? "LONG" : "SHORT", grade, kSetupNames[Clamp(g.type, 0, SETUP_COUNT - 1)], g.rr, PXS(g.stop), sz);
			else sprintf_s(buf, sizeof(buf), "%s %+.1fR | %s %c", g.resolved == 1 ? "WIN" : (g.resolved == -1 ? "LOSS" : "TIMEOUT"), g.resultR, kSetupNames[Clamp(g.type, 0, SETUP_COUNT - 1)], grade);
			const uint32_t lc = live ? (g.dir > 0 ? cLong : cShort) : (g.resolved == 1 ? cLong : (g.resolved == -1 ? cShort : cDim));
			SlotText(sc, sd.label, live ? g.idx : endIdx + 1, 0, live ? (g.dir > 0 ? g.t2 : g.stop) : g.stop, false, buf, lc, Max(7, fontPt - 1), live, 0, false, DT_LEFT | (live ? DT_BOTTOM : (g.dir > 0 ? DT_TOP : DT_BOTTOM)));
		}
	}

	// swing-delta numbers at confirmed pivots (no connecting lines)
	const float numOff = on[TL_NUMBERS] ? 0.2f : 0.0f;
	if (on[TL_SWING] && !FL.legs.empty() && !heavy) SlotKeep(T.swing, TermState::SWING_SLOTS);
	else if (on[TL_SWING] && !FL.legs.empty())
	{
		int slot = 0;
		for (int k = static_cast<int>(FL.legs.size()) - 1; k >= 0 && slot < TermState::SWING_SLOTS; --k)
		{
			const FlowState::Leg& L2 = FL.legs[k];
			if (lastClosed - L2.toIdx > 240) break;
			char d[24]; render::Abbrev(L2.delta, d, sizeof(d));
			sprintf_s(buf, sizeof(buf), "%s%s%s", L2.delta > 0 ? "+" : "", d, L2.divergence ? " !" : "");
			SlotText(sc, T.swing[slot++], L2.toIdx, 0, L2.up ? L2.toPrice + (0.2f + numOff) * atr : L2.toPrice - (0.2f + numOff) * atr, false, buf, L2.delta >= 0 ? cBull : cBear, fontPt + 1, true, 0, false, DT_CENTER | (L2.up ? DT_BOTTOM : DT_TOP));
		}
	}
	// per-bar delta numbers (volume lives in the strip); only when bars are wide enough, staggered on narrow bars,
	// and not when the footprint already prints delta/volume in text mode
	const bool numbersOn = on[TL_NUMBERS] && barPx >= 12 && lastClosed >= 0 && !(on[TL_FOOTPRINT] && barPx >= 36);
	if (numbersOn && !heavy) SlotKeep(T.num, TermState::NUM_SLOTS);
	else if (numbersOn)
	{
		const int N = Min(TermState::NUM_SLOTS, sc.Input[TI_NUM_BARS].GetInt());
		const float stag = (barPx < 26 && pillH > 0) ? 0.7f * pillH : 0.0f;
		int slot = 0;
		for (int i = Max(0, lastClosed - N + 1); i <= lastClosed && slot < TermState::NUM_SLOTS; ++i)
		{
			const float dl = FL.delta[i]; char d[16]; render::Abbrev(dl, d, sizeof(d));
			sprintf_s(buf, sizeof(buf), "%s%s", dl > 0 ? "+" : "", d);
			SlotText(sc, T.num[slot++], i, 0, sc.High[i] + 0.08f * atr + ((i & 1) ? stag : 0.0f), false, buf, dl >= 0 ? cBull : cBear, Max(6, fontPt - 2), false, 0, false, DT_CENTER | DT_BOTTOM);
		}
	}
	// regression channel of the active leg
	if (on[TL_CHANNEL] && FL.legReg.startIdx >= 0 && FL.legReg.sigma > 0 && lastClosed - FL.legReg.startIdx >= 5 && !heavy) SlotKeep(T.chan, TermState::CHAN_SLOTS);
	else if (on[TL_CHANNEL] && FL.legReg.startIdx >= 0 && FL.legReg.sigma > 0 && lastClosed - FL.legReg.startIdx >= 5)
	{
		const FlowState::LegReg& R = FL.legReg; const int ext = Max(2, Min(10, fillBars / 4));
		const uint32_t cc = R.slope > 0 ? cBull : (R.slope < 0 ? cBear : cNeu);
		const float yA = static_cast<float>(R.intercept), yB = static_cast<float>(R.intercept + R.slope * (lastClosed + ext - R.startIdx)); const float k2 = static_cast<float>(2.0 * R.sigma);
		SlotLine(sc, T.chan[0], R.startIdx, lastClosed + ext, yA, yB, cc, 1, LINESTYLE_SOLID);
		SlotLine(sc, T.chan[1], R.startIdx, lastClosed + ext, yA + k2, yB + k2, cc, 1, LINESTYLE_DOT);
		SlotLine(sc, T.chan[2], R.startIdx, lastClosed + ext, yA - k2, yB - k2, cc, 1, LINESTYLE_DOT);
	}
	// zones within 2 ATR
	if (on[TL_ZONES] && !heavy) SlotKeep(T.zone, TermState::ZONE_SLOTS);
	else if (on[TL_ZONES])
	{
		int slot = 0;
		const std::vector<Zone>* sets[4] = { &FL.absorbZones, &FL.imbZones, &A.liquidity, &FL.failedZones };
		const char* letters[4] = { "A", "I", "L", "F" };
		for (int s = 0; s < 4 && slot < TermState::ZONE_SLOTS; ++s)
			for (int z = static_cast<int>(sets[s]->size()) - 1; z >= 0 && slot < TermState::ZONE_SLOTS; --z)
			{
				const Zone& Z = (*sets[s])[z]; if (!Z.active) continue;
				const float mid = 0.5f * (Z.top + Z.bottom); if (fabs(mid - close) > 2.0f * atr) continue;
				SlotRect(sc, T.zone[slot++], Z.bornIdx, lineEnd, Z.top, Z.bottom, Z.dir > 0 ? cBull : (Z.dir < 0 ? cBear : cLevel), 82, letters[s]);
			}
	}
	// large-trade bubbles
	if (on[TL_BUBBLES] && !FL.bubbles.empty() && !heavy) SlotKeep(T.bubble, TermState::BUBBLE_SLOTS);
	else if (on[TL_BUBBLES] && !FL.bubbles.empty())
	{
		int slot = 0; double mx = 1;
		for (int q = static_cast<int>(FL.bubbles.size()) - 1; q >= 0 && FL.bubbles[q].idx >= lastClosed - 90; --q) mx = Max(mx, FL.bubbles[q].size);
		for (int q = static_cast<int>(FL.bubbles.size()) - 1; q >= 0 && slot < TermState::BUBBLE_SLOTS; --q)
		{
			const Bubble& b = FL.bubbles[q];
			if (b.idx < lastClosed - 90) break;
			if (fabs(b.price - close) > 2.0f * atr) continue;
			SlotMarker(sc, T.bubble[slot++], b.idx, b.price, MARKER_POINT, 4 + static_cast<int>(8.0 * sqrt(b.size / mx)), b.dir > 0 ? cBull : cBear);
		}
	}
	// absorption bubbles: one per absorption bar at the absorbed price, sized by that bar's volume z-score
	if (on[TL_BUBBLES] && !heavy) SlotKeep(T.absb, TermState::ABSB_SLOTS);
	else if (on[TL_BUBBLES] && lastClosed >= 0)
	{
		int slot = 0;
		for (int i = lastClosed; i >= Max(0, lastClosed - 150) && slot < TermState::ABSB_SLOTS; --i)
		{
			if (FL.absorbMark[i] == 0) continue;
			const float price = FL.absorbMark[i] > 0 ? sc.Low[i] : sc.High[i];
			if (fabs(price - close) > 3.0f * atr) continue;
			const int size = 8 + 4 * Clamp(static_cast<int>(FL.volZ[i] - 1.0f), 0, 4);
			SlotMarker(sc, T.absb[slot++], i, price, MARKER_POINT, size, FL.absorbMark[i] > 0 ? cBull : cBear);
		}
	}
	// trend lines: resistance through the last two valid swing highs (red), support through swing lows (green); dotted once broken
	if (on[TL_TRENDLINES] && !heavy) SlotKeep(T.tl, TermState::TLN_SLOTS);
	else if (on[TL_TRENDLINES] && lastClosed > 10)
	{
		const int sw = sc.Input[TI_TL_SWINGS].GetInt();
		TrendLine tl;
		if (FindTrendLine(sc, S, lastClosed, true, sw, tl)) SlotLine(sc, T.tl[0], tl.i1, lineEnd, tl.p1, tl.At(lineEnd), cBear, 1, tl.broken ? LINESTYLE_DOT : LINESTYLE_SOLID);
		if (FindTrendLine(sc, S, lastClosed, false, sw, tl)) SlotLine(sc, T.tl[1], tl.i1, lineEnd, tl.p1, tl.At(lineEnd), cBull, 1, tl.broken ? LINESTYLE_DOT : LINESTYLE_SOLID);
	}
	// event log: the last 6 distinct order-flow / structure events as a list at the end opposite the HUD, each with a dash marker at its price
	const int LOGN = TermState::NOTE_SLOTS / 2;
	if (on[TL_NOTES] && !heavy) SlotKeep(T.note, TermState::NOTE_SLOTS);
	else if (on[TL_NOTES])
	{
		const Event* pick[TermState::NOTE_SLOTS / 2]; int np = 0;
		for (int k = static_cast<int>(S.events.size()) - 1; k >= 0 && np < LOGN; --k)
		{
			const Event& e = S.events[k];
			if (e.kind == EV_SIGNAL) continue;
			if (lastClosed - e.idx > 240) break;
			bool dup = false; for (int q = 0; q < np; ++q) if (pick[q]->kind == e.kind && pick[q]->dir == e.dir && pick[q]->idx - e.idx <= 6) { dup = true; break; }
			if (dup) continue;
			pick[np++] = &e;
		}
		for (int q = 0; q < np; ++q)
		{
			const Event& e = *pick[q];
			const int ago = lastClosed - e.idx;
			if (ago <= 0) sprintf_s(buf, sizeof(buf), "%s %s", e.time, e.text); else sprintf_s(buf, sizeof(buf), "%s %s (%db)", e.time, e.text, ago);
			const uint32_t col = e.dir > 0 ? cBull : (e.dir < 0 ? cBear : cLevel);
			const float y = hudTop ? hudBase + pitch * static_cast<float>(LOGN - q) : 97.0f - pitch * static_cast<float>(q);
			SlotText(sc, T.note[q], -1, relX, y, true, buf, col, Max(7, fontPt - 1), q == 0, 0, false, DT_LEFT | DT_TOP);
			if (fabs(e.price - close) <= 3.0f * atr) SlotMarker(sc, T.note[LOGN + q], e.idx, e.price, MARKER_DASH, 8, col);
		}
	}
	// key session times: dotted vertical lines with a top label at the RTH open, IB end and RTH close of the last two sessions
	if (on[TL_KEYTIMES] && !heavy) SlotKeep(T.kt, TermState::KT_SLOTS);
	else if (on[TL_KEYTIMES] && lastClosed > 0)
	{
		const BaseState& B = S.base; const int secPerBar = Max(1, sc.SecondsPerBar); const int ibBars = S.params.auction.ibMinutes * 60 / secPerBar;
		int slot = 0, sessions = 0;
		for (int i = lastClosed; i > 0 && slot + 1 < TermState::KT_SLOTS && sessions < 2 && lastClosed - i < 4000; --i)
		{
			if (B.isRth[i] && !B.isRth[i - 1])
			{
				SlotVLine(sc, T.kt[slot], i, cDim, 1, LINESTYLE_DOT); SlotText(sc, T.kt[slot + 1], i, 0, 99.0f, true, " OPEN", cDim, Max(7, fontPt - 2), false, 0, false, DT_LEFT | DT_TOP); slot += 2;
				if (ibBars > 0 && i + ibBars <= lastClosed && slot + 1 < TermState::KT_SLOTS) { SlotVLine(sc, T.kt[slot], i + ibBars, cDim, 1, LINESTYLE_DOT); SlotText(sc, T.kt[slot + 1], i + ibBars, 0, 99.0f, true, " IB end", cDim, Max(7, fontPt - 2), false, 0, false, DT_LEFT | DT_TOP); slot += 2; }
				++sessions;
			}
			else if (!B.isRth[i] && B.isRth[i - 1])
			{
				SlotVLine(sc, T.kt[slot], i, cDim, 1, LINESTYLE_DOT); SlotText(sc, T.kt[slot + 1], i, 0, 99.0f, true, " CLOSE", cDim, Max(7, fontPt - 2), false, 0, false, DT_LEFT | DT_TOP); slot += 2;
			}
		}
	}
	// projection: expected path from the last bar into the fill space with the empirical odds
	if (on[TL_PROJ] && lastClosed >= 0 && (H.regime == RG_TREND_UP || H.regime == RG_TREND_DOWN || H.regime == RG_BALANCE))
	{
		float w1 = 0, w2 = 0; int dir = 0; int setup = SETUP_NONE;
		const Signal* live = (!D.signals.empty() && lastClosed - D.signals.back().idx <= 5 && D.signals.back().resolved == 0) ? &D.signals.back() : nullptr;
		if (live) { dir = live->dir; w1 = live->t1; w2 = live->t2; setup = live->type; }
		else if (H.regime == RG_BALANCE) { const float poc = A.poc.empty() ? 0.0f : A.poc[lastClosed]; dir = poc > close ? 1 : -1; w1 = poc; w2 = dir > 0 ? H.resPrice : H.supPrice; setup = SETUP_VALUE_EDGE; }
		else { dir = H.regime == RG_TREND_UP ? 1 : -1; w1 = dir > 0 ? H.supPrice : H.resPrice; w2 = dir > 0 ? H.resPrice : H.supPrice; setup = SETUP_TREND_PULLBACK; }
		if (w1 > 0)
		{
			const int stepB = Max(2, Min(8, fillBars / 5));
			SlotLine(sc, T.proj[0], n - 1, n - 1 + stepB, sc.Close[lastClosed], w1, dir > 0 ? cBull : cBear, 1, LINESTYLE_DASH);
			if (w2 > 0 && fabs(w2 - w1) > tick) SlotLine(sc, T.proj[1], n - 1 + stepB, n - 1 + 2 * stepB, w1, w2, dir > 0 ? cBull : cBear, 1, LINESTYLE_DASH);
			int nn = 0, wins = 0;
			for (int g = 0; g < 3; ++g) { if (live && (g + 1) != live->grade) continue; const SetupStats& st = S.val.stats3[setup][Clamp(H.regime, 0, 4)][g]; nn += st.wins + st.losses; wins += st.wins; }
			if (nn < 10) { const SetupStats& st = S.val.stats[setup]; nn = st.wins + st.losses; wins = st.wins; }
			const char* what = live ? "signal" : (H.regime == RG_BALANCE ? "rotation" : "pullback+go");
			if (nn > 0) sprintf_s(buf, sizeof(buf), "%s P(T1) %.0f%% n=%d%s", what, 100.0 * wins / Max(1, nn), nn, nn < S.params.val.minSample ? " (low conf.)" : "");
			else sprintf_s(buf, sizeof(buf), "%s | no history yet", what);
			SlotText(sc, T.proj[2], n - 1 + stepB, 0, w1, false, buf, nn >= S.params.val.minSample ? cText : cDim, Max(7, fontPt - 1), false, 0, false, DT_LEFT | (dir > 0 ? DT_BOTTOM : DT_TOP));
		}
	}

	// HUD: compact lines (about 48 characters) in the empty space right of the last bar
	if (on[TL_HUD])
	{
		struct HudLine { char text[160]; uint32_t color; bool bold; bool pill; uint32_t back; int pt; };
		HudLine lines[TermState::HUD_SLOTS]; int nl2 = 0;
		HudLine L; L.pill = false; L.bold = false; L.pt = hudPt; L.back = 0; L.color = cText; L.text[0] = 0;
		#define HUD_PUSH() { if (nl2 < TermState::HUD_SLOTS) lines[nl2++] = L; L.pill = false; L.bold = false; L.pt = hudPt; L.back = 0; L.color = cText; }
		// 1 bias pill + score trend
		L.pill = true; L.bold = true; L.pt = hudPt + 3; L.color = cPanel; L.back = H.bias > 0 ? cBull : (H.bias < 0 ? cBear : cNeu);
		sprintf_s(L.text, sizeof(L.text), " %s  DCS %+.0f %s ", H.bias > 0 ? "LONG" : (H.bias < 0 ? "SHORT" : "NEUTRAL"), H.dcs, H.dcsTrend > 5 ? "^" : (H.dcsTrend < -5 ? "v" : "=")); HUD_PUSH();
		// 2 action pill: the one thing to do right now (daily risk guard overrides everything)
		{
			int kind = H.actionKind; char act[128];
			const int wS = S.params.dcs.tradeStartSec, wE = S.params.dcs.tradeEndSec; const int todNow = sc.GetCurrentDateTime().GetTimeInSeconds();
			const bool outsideWindow = wE > wS && (todNow < wS || todNow >= wE);
			if (limitHit) { kind = 3; sprintf_s(act, sizeof(act), " NO TRADE | daily limit hit (day $%+.0f) ", dayPnl); }
			else if (outsideWindow && (kind == 0 || kind == 3)) { kind = 3; sprintf_s(act, sizeof(act), " NO TRADE | outside window %02d:%02d-%02d:%02d ", wS / 3600, (wS % 3600) / 60, wE / 3600, (wE % 3600) / 60); }
			else sprintf_s(act, sizeof(act), " %s ", H.action[0] ? H.action : "WAIT");
			L.pill = true; L.bold = true; L.pt = hudPt + 1;
			if (kind == 1 || kind == 2) { L.back = cBull; L.color = cPanel; }
			else if (kind == -1 || kind == -2) { L.back = cBear; L.color = cPanel; }
			else if (kind == 3) { L.back = render::Blend(cBear, cPanel, 0.55f); L.color = cText; }
			else { L.back = render::Blend(cNeu, cPanel, 0.55f); L.color = cText; }
			strncpy_s(L.text, sizeof(L.text), act, _TRUNCATE); HUD_PUSH();
		}
		// 2 regime | open type | value migration
		L.color = H.regime == RG_TREND_UP ? cBull : (H.regime == RG_TREND_DOWN ? cBear : cText);
		sprintf_s(L.text, sizeof(L.text), "%s | %s | %s", kRegimeNames[Clamp(H.regime, 0, 4)], kOpenShort[Clamp(H.openType, 0, 7)], H.valueMig > 0.5f ? "value higher" : (H.valueMig < -0.5f ? "value lower" : "value overlap")); HUD_PUSH();
		// 3 (PRO) day type | group agreement | range vs ADR
		if (pro)
		{
			sprintf_s(L.text, sizeof(L.text), "%s | %d/%d groups agree | %s%.0f%% ADR", H.dayType[0] ? H.dayType : "--", H.agree, H.agreeN, H.adrPrev ? "prev RTH " : "range ", H.adrPct);
			L.color = H.agreeN > 0 && H.agree * 4 >= H.agreeN * 3 ? (H.dcs >= 0 ? cBull : cBear) : cDim; HUD_PUSH();
		}
		// 4-5 plan (two lines)
		L.bold = true; L.color = cVwap; strncpy_s(L.text, sizeof(L.text), H.stateLine[0] ? H.stateLine : "Warming up", _TRUNCATE); HUD_PUSH();
		L.color = cVwap; strncpy_s(L.text, sizeof(L.text), H.stateLine2[0] ? H.stateLine2 : "", _TRUNCATE); HUD_PUSH();
		// 6 MTF strip | leg quality | SMT
		{
			static const char* nm[4] = { "1m", "5m", "15m", "60m" };
			char mt[120] = "MTF"; for (int k = 0; k < 4; ++k) { char c[16]; sprintf_s(c, sizeof(c), " %s%s", nm[k], !H.mtfAvail[k] ? "." : (H.mtf[k] > 0 ? "+" : (H.mtf[k] < 0 ? "-" : "="))); strcat_s(mt, sizeof(mt), c); }
			char ex[48]; sprintf_s(ex, sizeof(ex), " | leg R2 %.2f%s", H.legR2, H.smt != 0 ? (H.smt > 0 ? " | SMT+" : " | SMT-") : ""); strcat_s(mt, sizeof(mt), ex);
			strncpy_s(L.text, sizeof(L.text), mt, _TRUNCATE); HUD_PUSH();
		}
		// 7 (PRO) order flow: CVD trend / z, bar delta, lead-lag
		if (pro)
		{
			const float dl = lastClosed >= 0 ? FL.delta[lastClosed] : 0.0f; const float dp = lastClosed >= 0 ? FL.deltaPct[lastClosed] : 0.0f;
			char d[16]; render::Abbrev(dl, d, sizeof(d));
			sprintf_s(L.text, sizeof(L.text), "CVD %s z%+.1f | delta %s%s (%+.0f%%)", H.cvdTrend > 0 ? "^" : (H.cvdTrend < 0 ? "v" : "="), H.cvdZ, dl > 0 ? "+" : "", d, dp * 100);
			L.color = H.cvdTrend > 0 ? cBull : (H.cvdTrend < 0 ? cBear : cText); HUD_PUSH();
		}
		// 9 (PRO) internals: the other feeds (relative strength signs), NYSE TICK, mega-cap leadership, lead / lag
		if (pro)
		{
			if (H.interConfigured == 0) { strcpy_s(L.text, sizeof(L.text), "Internals: set Chart Number inputs (ES / YM / TICK / mega caps)"); L.color = cDim; }
			else
			{
				static const char* S3[3] = { "-", "=", "+" };
				char t[160] = ""; char piece[40];
				if (H.esAvail) { sprintf_s(piece, sizeof(piece), "ES%s ", S3[Clamp(H.es, -1, 1) + 1]); strcat_s(t, sizeof(t), piece); }
				if (H.ymAvail) { sprintf_s(piece, sizeof(piece), "YM%s ", S3[Clamp(H.ym, -1, 1) + 1]); strcat_s(t, sizeof(t), piece); }
				if (H.rtyAvail) { sprintf_s(piece, sizeof(piece), "RTY%s ", S3[Clamp(H.rty, -1, 1) + 1]); strcat_s(t, sizeof(t), piece); }
				if (H.tickAvail) { sprintf_s(piece, sizeof(piece), "| TICK %+.2f ", H.tickVal); strcat_s(t, sizeof(t), piece); }
				if (H.megaCount > 0) { strcat_s(t, sizeof(t), "| "); for (int k = 0; k < H.megaCount && k < 4; ++k) { sprintf_s(piece, sizeof(piece), "%s%s ", H.megaNames[k], S3[Clamp(static_cast<int>(H.mega[k]), -1, 1) + 1]); strcat_s(t, sizeof(t), piece); } }
				if (H.leadLag[0] && strlen(t) + strlen(H.leadLag) < 50) { strcat_s(t, sizeof(t), "| "); strcat_s(t, sizeof(t), H.leadLag); }
				if (!t[0]) sprintf_s(t, sizeof(t), "Internals: %d/%d charts connected", H.interConnected, H.interConfigured);
				strncpy_s(L.text, sizeof(L.text), t, _TRUNCATE); L.color = cText;
			}
			HUD_PUSH();
		}
		// 8 nearest structural levels
		if (H.resPrice > 0 && H.supPrice > 0) sprintf_s(L.text, sizeof(L.text), "R %s %s +%dt | S %s %s -%dt", dcs_detail::LevelName(H.resKind), PXS(H.resPrice), static_cast<int>((H.resPrice - close) / tick + 0.5f), dcs_detail::LevelName(H.supKind), PXS(H.supPrice), static_cast<int>((close - H.supPrice) / tick + 0.5f));
		else strcpy_s(L.text, sizeof(L.text), "Levels: waiting for session data");
		L.color = cLevel; HUD_PUSH();
		// 9 (PRO) last event
		if (pro)
		{
			const Event* le = nullptr; for (int k = static_cast<int>(S.events.size()) - 1; k >= 0 && !le; --k) if (S.events[k].kind != EV_SIGNAL) le = &S.events[k];
			if (le) sprintf_s(L.text, sizeof(L.text), "last: %s %s (%db)", le->time, le->text, Max(0, lastClosed - le->idx)); else strcpy_s(L.text, sizeof(L.text), "last event: none yet");
			L.color = le ? (le->dir > 0 ? cBull : (le->dir < 0 ? cBear : cText)) : cDim; HUD_PUSH();
		}
		// 10 (PRO) scoreboard for the current setup
		if (pro)
		{
			const SetupStats& st = S.val.stats[Clamp(H.curSetup, 0, SETUP_COUNT - 1)];
			if (st.count > 0) sprintf_s(L.text, sizeof(L.text), "%s: %.0f%% win | %+.2fR | n=%d%s", kSetupNames[Clamp(H.curSetup, 0, SETUP_COUNT - 1)], 100.0 * st.wins / Max(1, st.wins + st.losses), st.sumR / st.count, st.count, st.count < S.params.val.minSample ? " (small n)" : "");
			else sprintf_s(L.text, sizeof(L.text), "Scoreboard: no resolved signals yet (%d logged)", H.signalsTotal);
			L.color = cDim; HUD_PUSH();
		}
		// 13 (PRO) position + daily risk: Sierra's trade position (live or simulated)
		if (pro)
		{
			char lim[40] = ""; if (dayLoss > 0) sprintf_s(lim, sizeof(lim), " | limit $-%.0f", dayLoss);
			if (!havePos) { strcpy_s(L.text, sizeof(L.text), "Position: no trade data (enable Trade Simulation Mode)"); L.color = cDim; }
			else if (limitHit) { sprintf_s(L.text, sizeof(L.text), " DAILY LIMIT HIT | day $%+.0f | trades %d | stand down ", dayPnl, pos.TotalTrades); L.pill = true; L.bold = true; L.back = cBear; L.color = cPanel; }
			else if (pos.PositionQuantity != 0) { sprintf_s(L.text, sizeof(L.text), "%s %.0f @ %s | open $%+.0f | day $%+.0f%s", pos.PositionQuantity > 0 ? "LONG" : "SHORT", fabs(pos.PositionQuantity), PXS(static_cast<float>(pos.AveragePrice)), pos.OpenProfitLoss, pos.DailyProfitLoss, lim); L.color = pos.OpenProfitLoss >= 0 ? cBull : cBear; }
			else { sprintf_s(L.text, sizeof(L.text), "FLAT | day $%+.0f | trades %d%s", pos.DailyProfitLoss, pos.TotalTrades, lim); L.color = cText; }
			HUD_PUSH();
		}
		// 14 (PRO) session clock | bar countdown | symbol
		if (pro)
		{
			const int tod = sc.GetCurrentDateTime().GetTimeInSeconds(); const BaseParams& BP = S.params.base; const int ibEnd = BP.rthStartSec + S.params.auction.ibMinutes * 60;
			int secs; const char* what;
			if (tod < BP.rthStartSec) { secs = BP.rthStartSec - tod; what = "to RTH open"; } else if (tod < ibEnd) { secs = ibEnd - tod; what = "to IB end"; } else if (tod < BP.rthEndSec) { secs = BP.rthEndSec - tod; what = "to close"; } else { secs = 86400 - tod + BP.rthStartSec; what = "to RTH open"; }
			const int cd = sc.GetLatestBarCountdownAsInteger != nullptr ? sc.GetLatestBarCountdownAsInteger() : 0;
			sprintf_s(L.text, sizeof(L.text), "%dh%02dm %s | bar %d:%02d | %s", secs / 3600, (secs % 3600) / 60, what, cd / 60, cd % 60, sc.Symbol.GetChars());
			L.color = cDim; HUD_PUSH();
		}
		// 12 health or the first warning
		if (S.warn.text[0]) { const char* e = strchr(S.warn.text, '\n'); size_t len = static_cast<size_t>((e ? e : S.warn.text + strlen(S.warn.text)) - S.warn.text); if (len > 52) len = 52; strncpy_s(L.text, sizeof(L.text), S.warn.text, len); L.color = cVwap; }
		else
		{
			char fs[40] = ""; if (fillBars < fillNeeded) sprintf_s(fs, sizeof(fs), " | Fill Space >= %d", fillNeeded);
			sprintf_s(L.text, sizeof(L.text), "%s | %s | mkt %d/%d | %.1f ms | reg %dpx%s", S.warn.vapOff ? "VAP off" : "VAP on", S.warn.noDepth ? "depth off" : "depth on", H.interConnected, H.interConfigured, H.updateMs, T.regionH, fs); L.color = cDim; L.pt = Max(7, hudPt - 1);
		}
		HUD_PUSH();
		#undef HUD_PUSH
		for (int k = 0; k < nl2; ++k)
		{
			const float y = k == 0 ? hudY0 : hudY0 - pillPitch - pitch * static_cast<float>(k - 1);
			SlotText(sc, T.hud[k], -1, relX, y, true, lines[k].text, lines[k].color, lines[k].pt, lines[k].bold, lines[k].back, lines[k].pill, DT_LEFT | DT_TOP);
		}
	}
	SlotFlush(sc, T.all, TermState::SLOTS, false);
	#undef PXS

	// ---- alerts: newest closed bar only, real time only, A/B by default ----
	DcsState& DW = S.dcs;
	const int total = static_cast<int>(DW.signals.size());
	if (sc.Input[TI_ALERTS].GetYesNo() && total > 0 && !limitHit && !sc.IsFullRecalculation && sc.DownloadingHistoricalData == 0 && !sc.IsReplayRunning())
	{
		const Signal& g = DW.signals[total - 1];
		if (g.idx == n - 2 && DW.lastAlertIdx != g.idx && g.grade <= S.params.dcs.alertMinGrade)
		{
			DW.lastAlertIdx = g.idx;
			SCString msg; msg.Format("NQ Edge %s @ %s stop %s T1 %s T2 %s", g.label, sc.FormatGraphValue(g.entry, sc.BaseGraphValueFormat).GetChars(),
				sc.FormatGraphValue(g.stop, sc.BaseGraphValueFormat).GetChars(), sc.FormatGraphValue(g.t1, sc.BaseGraphValueFormat).GetChars(), sc.FormatGraphValue(g.t2, sc.BaseGraphValueFormat).GetChars());
			if (g.size > 0) { SCString sz; sz.Format(" size %dx", g.size); msg += sz; }
			const int snd = sc.Input[TI_SOUND].GetAlertSoundNumber();
			if (snd > 0) sc.SetAlert(snd - 1, g.idx, msg); else sc.AddAlertLine(msg, 1);
		}
	}
}

// --- 10. NQ Edge Flow series: order-flow candlesticks + CVD / delta panels (any chart) ------------
// Light studies for the companion charts. They run the base / auction / VWAP / flow engines only (no DCS, no HUD) and
// draw with subgraphs, so there are no drawing objects to manage and nothing to clean up. Engine parameters come from
// the Terminal when it is on the same chart, otherwise from Flow Candles' inputs; the two panels are pure readers.
enum FlowCandleInput
{
	FCI_RTH_START = 0, FCI_RTH_END, FCI_DAY_START, FCI_ATR_LEN, FCI_SWING_N, FCI_CVD_RESET, FCI_ABS_Z, FCI_IMB_RATIO, FCI_IMB_STACK, FCI_LT_PCT, FCI_LT_MIN,
	FCI_COLOR_MODE, FCI_SHOW_ABS, FCI_SHOW_EXH, FCI_SHOW_TRAP, FCI_SHOW_DIV, FCI_SHOW_IMB, FCI_SHOW_POC, FCI_SHOW_BUBBLES, FCI_BUBBLE_BARS, FCI_SHOW_DELTA, FCI_SHOW_VOL,
	FCI_ALERT, FCI_SOUND,
	FCI_C_BULL, FCI_C_BEAR, FCI_C_NEU, FCI_C_GOLD, FCI_C_MAGENTA, FCI_C_DIM, FCI_SHOW_TL, FCI_TL_SWINGS, FCI_ABS_STYLE, FCI_COUNT
};
enum FlowCandleSubgraph
{
	FCS_CANDLE = 0, FCS_FORMING, FCS_ABS_BUY, FCS_ABS_SELL, FCS_EXH_TOP, FCS_EXH_BOT, FCS_TRAP_LONGS, FCS_TRAP_SHORTS, FCS_DIV_UP, FCS_DIV_DN,
	FCS_IMB_BUY, FCS_IMB_SELL, FCS_POC, FCS_BUB_BUY_S, FCS_BUB_BUY_M, FCS_BUB_BUY_L, FCS_BUB_SELL_S, FCS_BUB_SELL_M, FCS_BUB_SELL_L,
	FCS_DELTA_TXT, FCS_VOL_TXT, FCS_H_DELTA_PCT, FCS_H_VOLZ, FCS_H_CVD,
	FCS_TL_RES, FCS_TL_SUP, FCS_ABSB_BUY_S, FCS_ABSB_BUY_M, FCS_ABSB_BUY_L, FCS_ABSB_SELL_S, FCS_ABSB_SELL_M, FCS_ABSB_SELL_L, FCS_COUNT
};

SCSFExport scsf_NQEdge_FlowCandles(SCStudyInterfaceRef sc)
{
	using namespace nqe;
	if (sc.SetDefaults)
	{
		sc.GraphName = "NQ Edge Flow Candles";
		sc.StudyDescription = "Order-flow candlesticks for any chart: bars coloured by a delta gradient (brighter on high volume), absorption diamonds, exhaustion triangles, trapped-trader crosses, CVD-divergence marks, stacked-imbalance dashes, the bar's POC, buy/sell bubbles in three sizes, delta and volume numbers, optional alerts. Shares the NQ Edge engines with the Terminal when both are on the chart.";
		sc.GraphRegion = 0; sc.AutoLoop = 0; sc.CalculationPrecedence = LOW_PREC_LEVEL; sc.ValueFormat = 0; sc.ScaleRangeType = SCALE_SAMEASREGION; sc.DrawZeros = 0; sc.MaintainVolumeAtPriceData = 1;
		const char* names[FCS_COUNT] = { "Flow Candle", "Forming Bar", "Absorption (buyers)", "Absorption (sellers)", "Exhaustion Top", "Exhaustion Bottom", "Trapped Longs", "Trapped Shorts", "CVD Divergence Up", "CVD Divergence Down",
			"Imbalance Stack (buy)", "Imbalance Stack (sell)", "Bar POC", "Buy Bubble S", "Buy Bubble M", "Buy Bubble L", "Sell Bubble S", "Sell Bubble M", "Sell Bubble L", "Delta", "Volume", "h.Delta %", "h.Volume Z", "h.CVD",
			"Trend Line (resistance)", "Trend Line (support)", "Absorption Bubble Buy S", "Absorption Bubble Buy M", "Absorption Bubble Buy L", "Absorption Bubble Sell S", "Absorption Bubble Sell M", "Absorption Bubble Sell L" };
		for (int k = 0; k < FCS_COUNT; ++k) { sc.Subgraph[k].Name = names[k]; sc.Subgraph[k].DrawStyle = DRAWSTYLE_IGNORE; sc.Subgraph[k].DrawZeros = 0; sc.Subgraph[k].LineWidth = 1; }
		sc.Subgraph[FCS_CANDLE].DrawStyle = DRAWSTYLE_COLOR_BAR; sc.Subgraph[FCS_CANDLE].PrimaryColor = RGB(138, 147, 166);
		sc.Subgraph[FCS_FORMING].DrawStyle = DRAWSTYLE_COLOR_BAR_HOLLOW; sc.Subgraph[FCS_FORMING].PrimaryColor = RGB(138, 147, 166);
		sc.Subgraph[FCS_ABS_BUY].DrawStyle = DRAWSTYLE_DIAMOND; sc.Subgraph[FCS_ABS_BUY].LineWidth = 6; sc.Subgraph[FCS_ABS_BUY].PrimaryColor = RGB(0, 200, 150);
		sc.Subgraph[FCS_ABS_SELL].DrawStyle = DRAWSTYLE_DIAMOND; sc.Subgraph[FCS_ABS_SELL].LineWidth = 6; sc.Subgraph[FCS_ABS_SELL].PrimaryColor = RGB(255, 77, 94);
		sc.Subgraph[FCS_EXH_TOP].DrawStyle = DRAWSTYLE_TRIANGLE_DOWN; sc.Subgraph[FCS_EXH_TOP].LineWidth = 5; sc.Subgraph[FCS_EXH_TOP].PrimaryColor = RGB(255, 200, 87);
		sc.Subgraph[FCS_EXH_BOT].DrawStyle = DRAWSTYLE_TRIANGLE_UP; sc.Subgraph[FCS_EXH_BOT].LineWidth = 5; sc.Subgraph[FCS_EXH_BOT].PrimaryColor = RGB(255, 200, 87);
		sc.Subgraph[FCS_TRAP_LONGS].DrawStyle = DRAWSTYLE_X; sc.Subgraph[FCS_TRAP_LONGS].LineWidth = 5; sc.Subgraph[FCS_TRAP_LONGS].PrimaryColor = RGB(255, 77, 94);
		sc.Subgraph[FCS_TRAP_SHORTS].DrawStyle = DRAWSTYLE_X; sc.Subgraph[FCS_TRAP_SHORTS].LineWidth = 5; sc.Subgraph[FCS_TRAP_SHORTS].PrimaryColor = RGB(0, 200, 150);
		sc.Subgraph[FCS_DIV_UP].DrawStyle = DRAWSTYLE_PLUS; sc.Subgraph[FCS_DIV_UP].LineWidth = 6; sc.Subgraph[FCS_DIV_UP].PrimaryColor = RGB(214, 93, 255);
		sc.Subgraph[FCS_DIV_DN].DrawStyle = DRAWSTYLE_PLUS; sc.Subgraph[FCS_DIV_DN].LineWidth = 6; sc.Subgraph[FCS_DIV_DN].PrimaryColor = RGB(214, 93, 255);
		sc.Subgraph[FCS_IMB_BUY].DrawStyle = DRAWSTYLE_RIGHT_PRICE_BAR_DASH; sc.Subgraph[FCS_IMB_BUY].LineWidth = 2; sc.Subgraph[FCS_IMB_BUY].PrimaryColor = RGB(0, 200, 150);
		sc.Subgraph[FCS_IMB_SELL].DrawStyle = DRAWSTYLE_LEFT_PRICE_BAR_DASH; sc.Subgraph[FCS_IMB_SELL].LineWidth = 2; sc.Subgraph[FCS_IMB_SELL].PrimaryColor = RGB(255, 77, 94);
		sc.Subgraph[FCS_POC].DrawStyle = DRAWSTYLE_DASH; sc.Subgraph[FCS_POC].LineWidth = 2; sc.Subgraph[FCS_POC].PrimaryColor = RGB(255, 200, 87);
		const int bubW[3] = { 4, 7, 10 };
		for (int k = 0; k < 3; ++k)
		{
			sc.Subgraph[FCS_BUB_BUY_S + k].DrawStyle = DRAWSTYLE_POINT; sc.Subgraph[FCS_BUB_BUY_S + k].LineWidth = bubW[k]; sc.Subgraph[FCS_BUB_BUY_S + k].PrimaryColor = RGB(0, 200, 150);
			sc.Subgraph[FCS_BUB_SELL_S + k].DrawStyle = DRAWSTYLE_POINT; sc.Subgraph[FCS_BUB_SELL_S + k].LineWidth = bubW[k]; sc.Subgraph[FCS_BUB_SELL_S + k].PrimaryColor = RGB(255, 77, 94);
		}
		sc.Subgraph[FCS_TL_RES].DrawStyle = DRAWSTYLE_LINE; sc.Subgraph[FCS_TL_RES].LineWidth = 1; sc.Subgraph[FCS_TL_RES].PrimaryColor = RGB(255, 77, 94);
		sc.Subgraph[FCS_TL_SUP].DrawStyle = DRAWSTYLE_LINE; sc.Subgraph[FCS_TL_SUP].LineWidth = 1; sc.Subgraph[FCS_TL_SUP].PrimaryColor = RGB(0, 200, 150);
		const int absW[3] = { 8, 12, 16 };
		for (int k = 0; k < 3; ++k)
		{
			sc.Subgraph[FCS_ABSB_BUY_S + k].DrawStyle = DRAWSTYLE_POINT; sc.Subgraph[FCS_ABSB_BUY_S + k].LineWidth = absW[k]; sc.Subgraph[FCS_ABSB_BUY_S + k].PrimaryColor = RGB(0, 200, 150);
			sc.Subgraph[FCS_ABSB_SELL_S + k].DrawStyle = DRAWSTYLE_POINT; sc.Subgraph[FCS_ABSB_SELL_S + k].LineWidth = absW[k]; sc.Subgraph[FCS_ABSB_SELL_S + k].PrimaryColor = RGB(255, 77, 94);
		}
		sc.Subgraph[FCS_DELTA_TXT].DrawStyle = DRAWSTYLE_VALUE_ON_HIGH; sc.Subgraph[FCS_DELTA_TXT].PrimaryColor = RGB(0, 200, 150);
		sc.Subgraph[FCS_VOL_TXT].DrawStyle = DRAWSTYLE_VALUE_ON_LOW; sc.Subgraph[FCS_VOL_TXT].PrimaryColor = RGB(120, 128, 145);

		sc.Input[FCI_RTH_START].Name = "Session: RTH Start"; sc.Input[FCI_RTH_START].SetTime(HMS_TIME(9, 30, 0));
		sc.Input[FCI_RTH_END].Name = "Session: RTH End"; sc.Input[FCI_RTH_END].SetTime(HMS_TIME(16, 0, 0));
		sc.Input[FCI_DAY_START].Name = "Session: Trading Day Start (evening open)"; sc.Input[FCI_DAY_START].SetTime(HMS_TIME(18, 0, 0));
		NQE_INT_INPUT(FCI_ATR_LEN, "Session: ATR Length", 14, 2, 500);
		NQE_INT_INPUT(FCI_SWING_N, "Structure: Swing Strength Bars", 5, 2, 50);
		sc.Input[FCI_CVD_RESET].Name = "CVD Reset"; sc.Input[FCI_CVD_RESET].SetCustomInputStrings("RTH Open;Trading Day Start;Never"); sc.Input[FCI_CVD_RESET].SetCustomInputIndex(0);
		NQE_FLT_INPUT(FCI_ABS_Z, "Flow: Absorption Volume Z >=", 2.0, 0.5, 10.0);
		NQE_FLT_INPUT(FCI_IMB_RATIO, "Flow: Imbalance Ratio %", 300.0, 150.0, 2000.0);
		NQE_INT_INPUT(FCI_IMB_STACK, "Flow: Stacked Levels >=", 3, 2, 20);
		NQE_FLT_INPUT(FCI_LT_PCT, "Bubbles: Large Trade Percentile", 99.0, 80.0, 99.99);
		NQE_INT_INPUT(FCI_LT_MIN, "Bubbles: Min Trade Size", 20, 1, 100000);
		sc.Input[FCI_COLOR_MODE].Name = "Candle Colour"; sc.Input[FCI_COLOR_MODE].SetCustomInputStrings("Delta gradient (brighter on volume);Delta sign;Up / down;Off"); sc.Input[FCI_COLOR_MODE].SetCustomInputIndex(0);
		NQE_YESNO_INPUT(FCI_SHOW_ABS, "Show: Absorption Diamonds", 1);
		NQE_YESNO_INPUT(FCI_SHOW_EXH, "Show: Exhaustion Triangles", 1);
		NQE_YESNO_INPUT(FCI_SHOW_TRAP, "Show: Trapped-Trader Crosses", 1);
		NQE_YESNO_INPUT(FCI_SHOW_DIV, "Show: CVD Divergence Marks", 1);
		NQE_YESNO_INPUT(FCI_SHOW_IMB, "Show: Stacked Imbalance Dashes", 1);
		NQE_YESNO_INPUT(FCI_SHOW_POC, "Show: Bar POC Dash", 1);
		NQE_YESNO_INPUT(FCI_SHOW_BUBBLES, "Show: Large-Trade Bubbles", 1);
		NQE_INT_INPUT(FCI_BUBBLE_BARS, "Bubbles: Lookback Bars", 200, 20, 2000);
		NQE_YESNO_INPUT(FCI_SHOW_DELTA, "Show: Delta Number Above Bar", 1);
		NQE_YESNO_INPUT(FCI_SHOW_VOL, "Show: Volume Number Below Bar", 0);
		NQE_YESNO_INPUT(FCI_ALERT, "Alert On Absorption / Trap / Divergence", 0);
		sc.Input[FCI_SOUND].Name = "Alert Sound Number"; sc.Input[FCI_SOUND].SetAlertSoundNumber(2);
		NQE_COLOR_INPUT(FCI_C_BULL, "Color: Bull", 0, 200, 150);
		NQE_COLOR_INPUT(FCI_C_BEAR, "Color: Bear", 255, 77, 94);
		NQE_COLOR_INPUT(FCI_C_NEU, "Color: Neutral", 110, 118, 134);
		NQE_COLOR_INPUT(FCI_C_GOLD, "Color: Exhaustion / POC", 255, 200, 87);
		NQE_COLOR_INPUT(FCI_C_MAGENTA, "Color: Divergence", 214, 93, 255);
		NQE_COLOR_INPUT(FCI_C_DIM, "Color: Volume Number", 120, 128, 145);
		NQE_YESNO_INPUT(FCI_SHOW_TL, "Show: Trend Lines (auto, from swings)", 1);
		NQE_INT_INPUT(FCI_TL_SWINGS, "Trend Lines: Swings Scanned", 8, 2, 30);
		sc.Input[FCI_ABS_STYLE].Name = "Absorption Marker"; sc.Input[FCI_ABS_STYLE].SetCustomInputStrings("Bubbles (sized by volume);Diamonds"); sc.Input[FCI_ABS_STYLE].SetCustomInputIndex(0);
		return;
	}
	if (sc.LastCallToFunction) { Release(sc, -1); return; }
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	ChartState& S = Acquire(sc);
	if (sc.MaintainVolumeAtPriceData == 0) { sc.MaintainVolumeAtPriceData = 1; sc.FlagToReloadChartData = 1; }
	if (!S.terminalPresent)
	{
		BaseParams bp{}; bp.rthStartSec = sc.Input[FCI_RTH_START].GetTime(); bp.rthEndSec = sc.Input[FCI_RTH_END].GetTime(); bp.dayStartSec = sc.Input[FCI_DAY_START].GetTime(); bp.atrLength = sc.Input[FCI_ATR_LEN].GetInt(); SetParams(S, E_BASE, S.params.base, bp);
		AuctionParams ap{}; ap.swingStrength = sc.Input[FCI_SWING_N].GetInt(); SetParams(S, E_AUCTION, S.params.auction, ap);
		FlowParams fp{}; fp.cvdReset = sc.Input[FCI_CVD_RESET].GetIndex(); fp.absorbVolZ = sc.Input[FCI_ABS_Z].GetFloat(); fp.imbRatioPct = sc.Input[FCI_IMB_RATIO].GetFloat(); fp.imbStackLevels = sc.Input[FCI_IMB_STACK].GetInt();
		fp.largePercentile = sc.Input[FCI_LT_PCT].GetFloat(); fp.largeMinSize = sc.Input[FCI_LT_MIN].GetInt(); SetParams(S, E_FLOW, S.params.flow, fp);
		if (sc.IsFullRecalculation && sc.UpdateStartIndex == 0) ResetFrom(S, E_BASE);
	}
	CheckDataStamp(sc, S);
	EnsureFlow(sc, S);
	const int n = sc.ArraySize; if (n <= 0) return;
	const FlowState& F = S.flow;
	const uint32_t cBull = sc.Input[FCI_C_BULL].GetColor(), cBear = sc.Input[FCI_C_BEAR].GetColor(), cNeu = sc.Input[FCI_C_NEU].GetColor(), cGold = sc.Input[FCI_C_GOLD].GetColor(), cMag = sc.Input[FCI_C_MAGENTA].GetColor(), cDim = sc.Input[FCI_C_DIM].GetColor();
	sc.Subgraph[FCS_ABS_BUY].PrimaryColor = cBull; sc.Subgraph[FCS_ABS_SELL].PrimaryColor = cBear; sc.Subgraph[FCS_EXH_TOP].PrimaryColor = sc.Subgraph[FCS_EXH_BOT].PrimaryColor = cGold;
	sc.Subgraph[FCS_TRAP_LONGS].PrimaryColor = cBear; sc.Subgraph[FCS_TRAP_SHORTS].PrimaryColor = cBull; sc.Subgraph[FCS_DIV_UP].PrimaryColor = sc.Subgraph[FCS_DIV_DN].PrimaryColor = cMag;
	sc.Subgraph[FCS_IMB_BUY].PrimaryColor = cBull; sc.Subgraph[FCS_IMB_SELL].PrimaryColor = cBear; sc.Subgraph[FCS_POC].PrimaryColor = cGold; sc.Subgraph[FCS_VOL_TXT].PrimaryColor = cDim;
	for (int k = 0; k < 3; ++k) { sc.Subgraph[FCS_BUB_BUY_S + k].PrimaryColor = cBull; sc.Subgraph[FCS_BUB_SELL_S + k].PrimaryColor = cBear; sc.Subgraph[FCS_ABSB_BUY_S + k].PrimaryColor = cBull; sc.Subgraph[FCS_ABSB_SELL_S + k].PrimaryColor = cBear; }
	sc.Subgraph[FCS_TL_RES].PrimaryColor = cBear; sc.Subgraph[FCS_TL_SUP].PrimaryColor = cBull;
	const int absStyle = sc.Input[FCI_ABS_STYLE].GetIndex();
	const int mode = sc.Input[FCI_COLOR_MODE].GetIndex();
	const bool showAbs = sc.Input[FCI_SHOW_ABS].GetYesNo() != 0, showExh = sc.Input[FCI_SHOW_EXH].GetYesNo() != 0, showTrap = sc.Input[FCI_SHOW_TRAP].GetYesNo() != 0, showDiv = sc.Input[FCI_SHOW_DIV].GetYesNo() != 0;
	const bool showImb = sc.Input[FCI_SHOW_IMB].GetYesNo() != 0, showPoc = sc.Input[FCI_SHOW_POC].GetYesNo() != 0, showBub = sc.Input[FCI_SHOW_BUBBLES].GetYesNo() != 0, showDelta = sc.Input[FCI_SHOW_DELTA].GetYesNo() != 0, showVol = sc.Input[FCI_SHOW_VOL].GetYesNo() != 0;
	sc.Subgraph[FCS_CANDLE].DrawStyle = mode == 3 ? DRAWSTYLE_IGNORE : DRAWSTYLE_COLOR_BAR; sc.Subgraph[FCS_FORMING].DrawStyle = mode == 3 ? DRAWSTYLE_IGNORE : DRAWSTYLE_COLOR_BAR_HOLLOW;
	sc.Subgraph[FCS_DELTA_TXT].DrawStyle = showDelta ? DRAWSTYLE_VALUE_ON_HIGH : DRAWSTYLE_IGNORE; sc.Subgraph[FCS_VOL_TXT].DrawStyle = showVol ? DRAWSTYLE_VALUE_ON_LOW : DRAWSTYLE_IGNORE;
	int& gen = sc.GetPersistentInt(11);
	int start = Min(sc.UpdateStartIndex, S.flow.dirtyFrom); S.flow.dirtyFrom = INT_MAX;
	if (gen != S.flow.generation) { start = 0; gen = S.flow.generation; }
	if (start < 0) start = 0;
	const bool haveVap = sc.VolumeAtPriceForBars != nullptr;
	const int nVap = haveVap ? static_cast<int>(sc.VolumeAtPriceForBars->GetNumberOfBars()) : 0;
	for (int i = Max(0, start - 1); i < n; ++i)
	{
		for (int k = 0; k < FCS_COUNT; ++k) sc.Subgraph[k][i] = 0;
		const bool forming = (i == n - 1);
		const float dp = F.deltaPct[i], vz = F.volZ[i], atr = AtrAt(S, i);
		uint32_t cc = cNeu;
		if (mode == 0) { const float t = Min(1.0f, static_cast<float>(fabs(dp)) / 0.35f); cc = render::Blend(cNeu, dp >= 0 ? cBull : cBear, 0.25f + 0.75f * t); if (vz >= 1.5f) cc = render::Blend(cc, RGB(255, 255, 255), 0.18f); }
		else if (mode == 1) cc = dp >= 0.05f ? cBull : (dp <= -0.05f ? cBear : cNeu);
		else if (mode == 2) cc = sc.Close[i] >= sc.Open[i] ? cBull : cBear;
		sc.Subgraph[FCS_CANDLE][i] = forming ? 0.0f : 1.0f; sc.Subgraph[FCS_CANDLE].DataColor[i] = cc;
		sc.Subgraph[FCS_FORMING][i] = forming ? 1.0f : 0.0f; sc.Subgraph[FCS_FORMING].DataColor[i] = cc;
		if (showAbs && F.absorbMark[i] != 0)
		{
			if (absStyle == 1) { if (F.absorbMark[i] > 0) sc.Subgraph[FCS_ABS_BUY][i] = sc.Low[i] - 0.2f * atr; else sc.Subgraph[FCS_ABS_SELL][i] = sc.High[i] + 0.2f * atr; }
			else { const int cls = vz >= 4.0f ? 2 : (vz >= 3.0f ? 1 : 0); sc.Subgraph[(F.absorbMark[i] > 0 ? FCS_ABSB_BUY_S : FCS_ABSB_SELL_S) + cls][i] = F.absorbMark[i] > 0 ? sc.Low[i] : sc.High[i]; }
		}
		if (showExh) { if (F.exhaustMark[i] == -1) sc.Subgraph[FCS_EXH_TOP][i] = sc.High[i] + 0.35f * atr; else if (F.exhaustMark[i] == 1) sc.Subgraph[FCS_EXH_BOT][i] = sc.Low[i] - 0.35f * atr; }
		if (showTrap) { if (F.trapMark[i] == -1) sc.Subgraph[FCS_TRAP_LONGS][i] = sc.High[i] + 0.5f * atr; else if (F.trapMark[i] == 1) sc.Subgraph[FCS_TRAP_SHORTS][i] = sc.Low[i] - 0.5f * atr; }
		if (showDiv) { if (F.divMark[i] == -1) sc.Subgraph[FCS_DIV_DN][i] = sc.High[i] + 0.65f * atr; else if (F.divMark[i] == 1) sc.Subgraph[FCS_DIV_UP][i] = sc.Low[i] - 0.65f * atr; }
		if (showImb && F.imbMark[i] != 0)
		{
			float buyAt = sc.Low[i], sellAt = sc.High[i];
			for (int z = static_cast<int>(F.imbZones.size()) - 1; z >= 0 && F.imbZones[z].bornIdx >= i; --z) if (F.imbZones[z].bornIdx == i) { if (F.imbZones[z].dir > 0) buyAt = F.imbZones[z].bottom; else sellAt = F.imbZones[z].top; }
			if (F.imbMark[i] == 1 || F.imbMark[i] == 2) sc.Subgraph[FCS_IMB_BUY][i] = buyAt;
			if (F.imbMark[i] == -1 || F.imbMark[i] == 2) sc.Subgraph[FCS_IMB_SELL][i] = sellAt;
		}
		if (showPoc && haveVap && i < nVap)
		{
			const int cnt = sc.VolumeAtPriceForBars->GetSizeAtBarIndex(i);
			double best = 0; int bestTicks = 0;
			for (int q = 0; q < cnt; ++q) { const s_VolumeAtPriceV2* pp = nullptr; if (sc.VolumeAtPriceForBars->GetVAPElementAtIndex(i, q, &pp) && pp && static_cast<double>(pp->Volume) > best) { best = static_cast<double>(pp->Volume); bestTicks = pp->PriceInTicks; } }
			if (best > 0) sc.Subgraph[FCS_POC][i] = bestTicks * S.tickSize;
		}
		if (showDelta) { sc.Subgraph[FCS_DELTA_TXT][i] = F.delta[i]; sc.Subgraph[FCS_DELTA_TXT].DataColor[i] = F.delta[i] >= 0 ? cBull : cBear; }
		if (showVol) sc.Subgraph[FCS_VOL_TXT][i] = sc.Volume[i];
		sc.Subgraph[FCS_H_DELTA_PCT][i] = dp * 100.0f; sc.Subgraph[FCS_H_VOLZ][i] = vz; sc.Subgraph[FCS_H_CVD][i] = F.cvd[i];
	}
	// bubbles: three size classes against the largest print in the lookback; re-laid over the lookback window every update
	if (showBub)
	{
		const int look = sc.Input[FCI_BUBBLE_BARS].GetInt();
		const int from = Max(0, n - look);
		for (int i = Max(from, Max(0, start - 1) < from ? from : 0); i < n; ++i) for (int k = 0; k < 6; ++k) sc.Subgraph[FCS_BUB_BUY_S + k][i] = 0;
		double mx = 0; for (int q = static_cast<int>(F.bubbles.size()) - 1; q >= 0 && F.bubbles[q].idx >= from; --q) mx = Max(mx, F.bubbles[q].size);
		for (int q = static_cast<int>(F.bubbles.size()) - 1; q >= 0 && F.bubbles[q].idx >= from; --q)
		{
			const Bubble& b = F.bubbles[q]; if (b.idx < 0 || b.idx >= n || mx <= 0) continue;
			const double r = b.size / mx; const int cls = r >= 0.66 ? 2 : (r >= 0.33 ? 1 : 0);
			const int sg = (b.dir > 0 ? FCS_BUB_BUY_S : FCS_BUB_SELL_S) + cls;
			if (sc.Subgraph[sg][b.idx] == 0) sc.Subgraph[sg][b.idx] = b.price;
		}
	}
	// trend lines from confirmed swings: resistance through the last two valid swing highs, support through swing lows
	if (sc.Input[FCI_SHOW_TL].GetYesNo())
	{
		const int sw = sc.Input[FCI_TL_SWINGS].GetInt(); const int lastClosed = n - 2; const int clearFrom = Max(0, n - 1500);
		for (int side = 0; side < 2; ++side)
		{
			const int sg = side == 0 ? FCS_TL_RES : FCS_TL_SUP;
			TrendLine tl; const bool ok = lastClosed > 10 && FindTrendLine(sc, S, lastClosed, side == 0, sw, tl);
			for (int i = clearFrom; i < n; ++i) sc.Subgraph[sg][i] = 0;
			if (!ok) continue;
			for (int i = Max(clearFrom, tl.i1); i < n; ++i) sc.Subgraph[sg][i] = tl.At(i);
			sc.Subgraph[sg].LineStyle = tl.broken ? LINESTYLE_DOT : LINESTYLE_SOLID;
		}
	}
	else { sc.Subgraph[FCS_TL_RES].DrawStyle = DRAWSTYLE_IGNORE; sc.Subgraph[FCS_TL_SUP].DrawStyle = DRAWSTYLE_IGNORE; }
	// alerts: the newest closed bar only, real time only
	if (sc.Input[FCI_ALERT].GetYesNo() && n >= 2 && !sc.IsFullRecalculation && sc.DownloadingHistoricalData == 0 && !sc.IsReplayRunning())
	{
		const int i = n - 2; int& last = sc.GetPersistentInt(12);
		if (last != i && (F.absorbMark[i] != 0 || F.trapMark[i] != 0 || F.divMark[i] != 0))
		{
			last = i;
			SCString msg; msg.Format("NQ Edge Flow %s: %s%s%s @ %s", sc.Symbol.GetChars(), F.absorbMark[i] > 0 ? "absorption (buyers) " : (F.absorbMark[i] < 0 ? "absorption (sellers) " : ""),
				F.trapMark[i] < 0 ? "trapped longs " : (F.trapMark[i] > 0 ? "trapped shorts " : ""), F.divMark[i] < 0 ? "CVD divergence (bearish)" : (F.divMark[i] > 0 ? "CVD divergence (bullish)" : ""), sc.FormatGraphValue(sc.Close[i], sc.BaseGraphValueFormat).GetChars());
			const int snd = sc.Input[FCI_SOUND].GetAlertSoundNumber();
			if (snd > 0) sc.SetAlert(snd - 1, i, msg); else sc.AddAlertLine(msg, 1);
		}
	}
}

// ---- Flow CVD panel: session cumulative delta with divergence dots ----
enum FlowCvdInput { FVI_SHOW_DIV = 0, FVI_C_UP, FVI_C_DN, FVI_C_DIV, FVI_C_ZERO, FVI_COUNT };
enum FlowCvdSubgraph { FVS_CVD = 0, FVS_DIV_BEAR, FVS_DIV_BULL, FVS_ZERO, FVS_H_CVDZ, FVS_COUNT };

SCSFExport scsf_NQEdge_FlowCVD(SCStudyInterfaceRef sc)
{
	using namespace nqe;
	if (sc.SetDefaults)
	{
		sc.GraphName = "NQ Edge Flow CVD";
		sc.StudyDescription = "Cumulative delta (reset per RTH session / trading day / never, set on Flow Candles or the Terminal) in its own panel, coloured by direction, with magenta dots where price made a new swing extreme on weaker delta (CVD divergence). Reader only: shares the engines of the other NQ Edge studies on the chart.";
		sc.GraphRegion = 1; sc.AutoLoop = 0; sc.CalculationPrecedence = LOW_PREC_LEVEL; sc.ValueFormat = 0; sc.ScaleRangeType = SCALE_AUTO; sc.DrawZeros = 0; sc.MaintainVolumeAtPriceData = 1;
		const char* names[FVS_COUNT] = { "CVD", "Divergence (bearish)", "Divergence (bullish)", "Zero", "h.CVD Z" };
		for (int k = 0; k < FVS_COUNT; ++k) { sc.Subgraph[k].Name = names[k]; sc.Subgraph[k].DrawStyle = DRAWSTYLE_IGNORE; sc.Subgraph[k].DrawZeros = 0; sc.Subgraph[k].LineWidth = 1; }
		sc.Subgraph[FVS_CVD].DrawStyle = DRAWSTYLE_LINE; sc.Subgraph[FVS_CVD].LineWidth = 2; sc.Subgraph[FVS_CVD].PrimaryColor = RGB(0, 200, 150); sc.Subgraph[FVS_CVD].DrawZeros = 1;
		sc.Subgraph[FVS_DIV_BEAR].DrawStyle = DRAWSTYLE_POINT; sc.Subgraph[FVS_DIV_BEAR].LineWidth = 8; sc.Subgraph[FVS_DIV_BEAR].PrimaryColor = RGB(214, 93, 255);
		sc.Subgraph[FVS_DIV_BULL].DrawStyle = DRAWSTYLE_POINT; sc.Subgraph[FVS_DIV_BULL].LineWidth = 8; sc.Subgraph[FVS_DIV_BULL].PrimaryColor = RGB(214, 93, 255);
		sc.Subgraph[FVS_ZERO].DrawStyle = DRAWSTYLE_LINE; sc.Subgraph[FVS_ZERO].LineWidth = 1; sc.Subgraph[FVS_ZERO].PrimaryColor = RGB(60, 66, 80); sc.Subgraph[FVS_ZERO].DrawZeros = 1;
		NQE_YESNO_INPUT(FVI_SHOW_DIV, "Show Divergence Dots", 1);
		NQE_COLOR_INPUT(FVI_C_UP, "Color: CVD Rising", 0, 200, 150);
		NQE_COLOR_INPUT(FVI_C_DN, "Color: CVD Falling", 255, 77, 94);
		NQE_COLOR_INPUT(FVI_C_DIV, "Color: Divergence", 214, 93, 255);
		NQE_COLOR_INPUT(FVI_C_ZERO, "Color: Zero Line", 60, 66, 80);
		return;
	}
	if (sc.LastCallToFunction) { Release(sc, -1); return; }
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	ChartState& S = Acquire(sc);
	if (sc.MaintainVolumeAtPriceData == 0) { sc.MaintainVolumeAtPriceData = 1; sc.FlagToReloadChartData = 1; }
	CheckDataStamp(sc, S);
	EnsureFlow(sc, S);
	const int n = sc.ArraySize; if (n <= 0) return;
	const FlowState& F = S.flow;
	const uint32_t cUp = sc.Input[FVI_C_UP].GetColor(), cDn = sc.Input[FVI_C_DN].GetColor(), cDiv = sc.Input[FVI_C_DIV].GetColor();
	sc.Subgraph[FVS_DIV_BEAR].PrimaryColor = sc.Subgraph[FVS_DIV_BULL].PrimaryColor = cDiv; sc.Subgraph[FVS_ZERO].PrimaryColor = sc.Input[FVI_C_ZERO].GetColor();
	const bool showDiv = sc.Input[FVI_SHOW_DIV].GetYesNo() != 0;
	int& gen = sc.GetPersistentInt(11);
	int start = Min(sc.UpdateStartIndex, S.flow.dirtyFrom);
	if (gen != S.flow.generation) { start = 0; gen = S.flow.generation; }
	if (start < 0) start = 0;
	for (int i = Max(0, start - 1); i < n; ++i)
	{
		sc.Subgraph[FVS_CVD][i] = F.cvd[i]; sc.Subgraph[FVS_CVD].DataColor[i] = (i > 0 && F.cvd[i] < F.cvd[i - 1]) ? cDn : cUp;
		sc.Subgraph[FVS_DIV_BEAR][i] = (showDiv && F.divMark[i] == -1) ? F.cvd[i] : 0.0f;
		sc.Subgraph[FVS_DIV_BULL][i] = (showDiv && F.divMark[i] == 1) ? F.cvd[i] : 0.0f;
		sc.Subgraph[FVS_ZERO][i] = 0; sc.Subgraph[FVS_H_CVDZ][i] = F.cvdZ[i];
	}
}

// ---- Flow Delta panel: delta histogram with absorption highlight, pressure line, high-volume shading ----
enum FlowDeltaInput { FDI_PRESS_LEN = 0, FDI_VOLZ_HI, FDI_C_UP, FDI_C_DN, FDI_C_ABS, FDI_C_TRAP, FDI_C_PRESS, FDI_C_HI, FDI_COUNT };
enum FlowDeltaSubgraph { FDS_BG = 0, FDS_DELTA, FDS_PRESSURE, FDS_ZERO, FDS_H_VOLZ, FDS_COUNT };

SCSFExport scsf_NQEdge_FlowDelta(SCStudyInterfaceRef sc)
{
	using namespace nqe;
	if (sc.SetDefaults)
	{
		sc.GraphName = "NQ Edge Flow Delta";
		sc.StudyDescription = "Per-bar delta histogram in its own panel: green / red by sign, gold on absorption bars, magenta on trapped-trader bars; a pressure line (EMA of delta); background shading on bars whose volume z-score is above the threshold. Reader only: shares the engines of the other NQ Edge studies on the chart.";
		sc.GraphRegion = 2; sc.AutoLoop = 0; sc.CalculationPrecedence = LOW_PREC_LEVEL; sc.ValueFormat = 0; sc.ScaleRangeType = SCALE_AUTO; sc.DrawZeros = 0; sc.MaintainVolumeAtPriceData = 1;
		const char* names[FDS_COUNT] = { "High-Volume Shade", "Delta", "Pressure (EMA of delta)", "Zero", "h.Volume Z" };
		for (int k = 0; k < FDS_COUNT; ++k) { sc.Subgraph[k].Name = names[k]; sc.Subgraph[k].DrawStyle = DRAWSTYLE_IGNORE; sc.Subgraph[k].DrawZeros = 0; sc.Subgraph[k].LineWidth = 1; }
		sc.Subgraph[FDS_BG].DrawStyle = DRAWSTYLE_BACKGROUND; sc.Subgraph[FDS_BG].PrimaryColor = RGB(34, 40, 56);
		sc.Subgraph[FDS_DELTA].DrawStyle = DRAWSTYLE_BAR; sc.Subgraph[FDS_DELTA].LineWidth = 2; sc.Subgraph[FDS_DELTA].PrimaryColor = RGB(0, 200, 150);
		sc.Subgraph[FDS_PRESSURE].DrawStyle = DRAWSTYLE_LINE; sc.Subgraph[FDS_PRESSURE].LineWidth = 2; sc.Subgraph[FDS_PRESSURE].PrimaryColor = RGB(255, 200, 87); sc.Subgraph[FDS_PRESSURE].DrawZeros = 1;
		sc.Subgraph[FDS_ZERO].DrawStyle = DRAWSTYLE_LINE; sc.Subgraph[FDS_ZERO].PrimaryColor = RGB(60, 66, 80); sc.Subgraph[FDS_ZERO].DrawZeros = 1;
		NQE_INT_INPUT(FDI_PRESS_LEN, "Pressure EMA Length", 10, 2, 200);
		NQE_FLT_INPUT(FDI_VOLZ_HI, "Shade Bars With Volume Z >=", 1.5, 0.5, 6.0);
		NQE_COLOR_INPUT(FDI_C_UP, "Color: Positive Delta", 0, 200, 150);
		NQE_COLOR_INPUT(FDI_C_DN, "Color: Negative Delta", 255, 77, 94);
		NQE_COLOR_INPUT(FDI_C_ABS, "Color: Absorption Bar", 255, 200, 87);
		NQE_COLOR_INPUT(FDI_C_TRAP, "Color: Trapped Bar", 214, 93, 255);
		NQE_COLOR_INPUT(FDI_C_PRESS, "Color: Pressure Line", 230, 234, 242);
		NQE_COLOR_INPUT(FDI_C_HI, "Color: High-Volume Shade", 34, 40, 56);
		return;
	}
	if (sc.LastCallToFunction) { Release(sc, -1); return; }
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	ChartState& S = Acquire(sc);
	if (sc.MaintainVolumeAtPriceData == 0) { sc.MaintainVolumeAtPriceData = 1; sc.FlagToReloadChartData = 1; }
	CheckDataStamp(sc, S);
	EnsureFlow(sc, S);
	const int n = sc.ArraySize; if (n <= 0) return;
	const FlowState& F = S.flow;
	const uint32_t cUp = sc.Input[FDI_C_UP].GetColor(), cDn = sc.Input[FDI_C_DN].GetColor(), cAbs = sc.Input[FDI_C_ABS].GetColor(), cTrap = sc.Input[FDI_C_TRAP].GetColor(), cPress = sc.Input[FDI_C_PRESS].GetColor(), cHi = sc.Input[FDI_C_HI].GetColor();
	sc.Subgraph[FDS_PRESSURE].PrimaryColor = cPress; sc.Subgraph[FDS_BG].PrimaryColor = cHi;
	const float hiZ = sc.Input[FDI_VOLZ_HI].GetFloat();
	const float alpha = 2.0f / (Max(2, sc.Input[FDI_PRESS_LEN].GetInt()) + 1.0f);
	int& gen = sc.GetPersistentInt(11);
	int start = Min(sc.UpdateStartIndex, S.flow.dirtyFrom);
	if (gen != S.flow.generation) { start = 0; gen = S.flow.generation; }
	if (start < 0) start = 0;
	for (int i = Max(0, start - 1); i < n; ++i)
	{
		const float d = F.delta[i];
		sc.Subgraph[FDS_DELTA][i] = d;
		sc.Subgraph[FDS_DELTA].DataColor[i] = F.absorbMark[i] != 0 ? cAbs : (F.trapMark[i] != 0 ? cTrap : (d >= 0 ? cUp : cDn));
		sc.Subgraph[FDS_PRESSURE][i] = (i == 0) ? d : sc.Subgraph[FDS_PRESSURE][i - 1] + alpha * (d - sc.Subgraph[FDS_PRESSURE][i - 1]);
		sc.Subgraph[FDS_BG][i] = F.volZ[i] >= hiZ ? 1.0f : 0.0f; sc.Subgraph[FDS_BG].DataColor[i] = cHi;
		sc.Subgraph[FDS_ZERO][i] = 0; sc.Subgraph[FDS_H_VOLZ][i] = F.volZ[i];
	}
}
