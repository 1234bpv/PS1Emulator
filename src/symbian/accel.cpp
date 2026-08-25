// Accelerometer on the Nokia RD sensor API.
//
// Neither the header nor the import library ships with the S60 3rd FP1 SDK,
// while the phone itself carries RRSensorApi.dll in ROM; and only one of the
// three target phones has the hardware at all. So the library is loaded by
// name and its two static entry points are taken by ordinal - 1 is
// FindSensorsL, 2 is NewL. Everything else on the class is virtual and is
// reached through the vtable, which is why the declarations below have to
// match the published header exactly.

#include <e32base.h>
#include <e32std.h>

const TInt KMaxSensorName = 128;

class TRRSensorInfo
	{
public:
	TInt iSensorCategory;
	TInt iSensorId;
	TBuf<KMaxSensorName> iSensorName;
	};

class TRRSensorEvent
	{
public:
	TInt iSensorData1;
	TInt iSensorData2;
	TInt iSensorData3;
	};

class MRRSensorDataListener
	{
public:
	virtual void HandleDataEventL(TRRSensorInfo aSensor, TRRSensorEvent aEvent) = 0;
	};

class CRRSensorApi : public CBase
	{
public:
	virtual void AddDataListener(MRRSensorDataListener *aListener) = 0;
	virtual void RemoveDataListener() = 0;
	virtual TInt SensorCommand(TInt &aCommand, TInt &aValue) = 0;
	};

typedef void (*TFnFindSensorsL)(RArray<TRRSensorInfo> &);
typedef CRRSensorApi *(*TFnSensorNewL)(TRRSensorInfo);

// The accelerometer among the sensor server's own sensors.
const TInt KAccelSensorId = 0x10273024;

class CAccelSink : public CBase, public MRRSensorDataListener
	{
public:
	void HandleDataEventL(TRRSensorInfo aSensor, TRRSensorEvent aEvent);
	TInt iX;
	TInt iY;
	TInt iZ;
	TInt iCount;
	};

void CAccelSink::HandleDataEventL(TRRSensorInfo aSensor, TRRSensorEvent aEvent)
	{
	(void)aSensor;
	iX = aEvent.iSensorData1;
	iY = aEvent.iSensorData2;
	iZ = aEvent.iSensorData3;
	iCount++;
	}

static RLibrary     g_lib;
static TBool        g_libOpen;
static CRRSensorApi *g_sensor;
static CAccelSink  *g_sink;

static void StartL()
	{
	TFnFindSensorsL find = (TFnFindSensorsL)g_lib.Lookup(1);
	TFnSensorNewL   mk   = (TFnSensorNewL)g_lib.Lookup(2);
	if (!find || !mk)
		User::Leave(KErrNotSupported);

	RArray<TRRSensorInfo> list;
	CleanupClosePushL(list);
	find(list);
	TInt idx = -1;
	for (TInt i = 0; i < list.Count(); i++)
		if (list[i].iSensorId == KAccelSensorId)
			{ idx = i; break; }
	if (idx < 0)
		User::Leave(KErrNotFound);

	CAccelSink *sink = new (ELeave) CAccelSink;
	CleanupStack::PushL(sink);
	CRRSensorApi *s = mk(list[idx]);
	if (!s)
		User::Leave(KErrGeneral);
	s->AddDataListener(sink);
	CleanupStack::Pop(sink);
	CleanupStack::PopAndDestroy(&list);
	g_sensor = s;
	g_sink = sink;
	}

// KErrNone once the sensor is delivering; KErrNotSupported on a phone without one.
extern "C" TInt accelStart(void)
	{
	if (g_sensor)
		return KErrNone;
	if (!g_libOpen)
		{
		_LIT(KDll, "RRSensorApi.dll");
		const TInt r = g_lib.Load(KDll, KNullUid);
		if (r != KErrNone)
			return r;
		g_libOpen = ETrue;
		}
	TRAPD(err, StartL());
	if (err != KErrNone)
		{
		delete g_sink; g_sink = NULL;
		g_sensor = NULL;
		}
	return err;
	}

extern "C" void accelStop(void)
	{
	if (g_sensor)
		{
		g_sensor->RemoveDataListener();
		delete g_sensor;
		g_sensor = NULL;
		}
	delete g_sink;
	g_sink = NULL;
	if (g_libOpen)
		{
		g_lib.Close();
		g_libOpen = EFalse;
		}
	}

// Latest reading; returns the number of events seen so far, or -1 if inactive.
extern "C" TInt accelRead(TInt *aX, TInt *aY, TInt *aZ)
	{
	if (!g_sink)
		return -1;
	if (aX) *aX = g_sink->iX;
	if (aY) *aY = g_sink->iY;
	if (aZ) *aZ = g_sink->iZ;
	return g_sink->iCount;
	}
