#include "NyWin32Capture.h"
#include <dshow.h>
#include <stdio.h>
#include <conio.h>
/**
コンパイルについて
このコードをコンパイルするためには、PSDK 7.0,PSDK6.1が必要です。
MicrosoftのWebサイトからダウンロードしてください。ダウンロードしたら、
PSDK7.0のdirectshow/baseclassesディレクトリから、.hと.cファイルを
extlib/BaseClassesWin32ディレクトリへコピーしてください。


qedit.hについて
Windows PSDK 7.0にはqedit.hが無いので、PSDK 6.1からコピーしてください。
さらにPSDK 6.1のqedit.hにはバグがあり、存在しないdxtrans.hを参照しているので、
その辺の問題を解消する必要があります。

具体的な方法は、以下URLが参考になります。
http://social.msdn.microsoft.com/forums/en-US/windowssdk/thread/ed097d2c-3d68-4f48-8448-277eaaf68252/


*/
#include <qedit.h>
#include <math.h>
#include <exception>
#include <vector>
#include <streams.h>
#include <assert.h>

using namespace std;

namespace NyWin32Capture
{
	/*	簡易スマートポインタ
	*/
	template <class T> class AutoReleaseComPtr
	{
	private:
		AutoReleaseComPtr(const AutoReleaseComPtr& );
		AutoReleaseComPtr& operator=(const AutoReleaseComPtr&);
	public:
		T* ptr;
		AutoReleaseComPtr<T>()
		{
			this->ptr=NULL;
		}
		AutoReleaseComPtr<T>(T* i_ptr)
		{
			this->ptr=i_ptr;
		}
		virtual ~AutoReleaseComPtr<T>()
		{
			if(this->ptr!=NULL)
			{
				this->ptr->Release();
				this->ptr=NULL;
			}
		}
		T* operator->()
		{
			return this->ptr;
		}
		operator T*() const
		{
			return this->ptr;
		}
		void detach(T** o_ptr)
		{
			if(o_ptr!=NULL){
				*o_ptr=this->ptr;
			}else{
				this->ptr->Release();
			}
			this->ptr=NULL;
		}
		void release()
		{
			this->ptr->Release();
			this->ptr=NULL;
		}
	};



	/*ICaptureGraphBuilder2からIAMStreamConfigインタフェイスを取得する。
	*/
	static bool mGetIAMStreamConfig(ICaptureGraphBuilder2* i_capbuilder,IBaseFilter* i_basefilter,const GUID& i_pin_category,IAMStreamConfig** o_config)
	{
		HRESULT hr;
		hr = i_capbuilder->FindInterface(&i_pin_category,0,i_basefilter,IID_IAMStreamConfig, (void **)o_config);
		if(FAILED(hr)){
			return false;
		}
		return true;
	}
	/*	Pinのカテゴリを取得する。
	*/
	static void mGetPinCategory(IPin* pPin,GUID& Category)
	{
		AutoReleaseComPtr<IKsPropertySet> pKs;
		HRESULT hr = pPin->QueryInterface(IID_IKsPropertySet, (void **)&(pKs.ptr));
		if (!SUCCEEDED(hr))
		{
			throw NyWin32CaptureException();
		}
		DWORD cbReturned;
		hr = pKs->Get(AMPROPSETID_Pin, AMPROPERTY_PIN_CATEGORY, NULL, 0, &Category, sizeof(GUID), &cbReturned);
	}

	/**/
	static IPin* mFindPinByDirection(IBaseFilter *pFilter, PIN_DIRECTION PinDir,int i_index)
	{
		int index=i_index;

		AutoReleaseComPtr<IEnumPins> pEnum;
		HRESULT hr = pFilter->EnumPins(&(pEnum.ptr));
		if (FAILED(hr))
		{
			return NULL;
		}
		AutoReleaseComPtr<IPin> pPin;
		while(pEnum->Next(1, &(pPin.ptr), 0) == S_OK)
		{
			PIN_DIRECTION PinDirThis;
			if(pPin->QueryDirection(&PinDirThis)!=S_OK){
				continue;
			}
			if(PinDir != PinDirThis)
			{
				pPin.release();
				continue;
			}
			if(index>0)
			{
				index--;
				pPin.release();
				continue;
			}
			IPin* result;
			pPin.detach(&result);
			return result;
		}
		return NULL;  
	}


	static IPin* mFindPinByCategory(IBaseFilter* i_filter,const GUID& Category)
	{
		HRESULT hr;
		AutoReleaseComPtr<IEnumPins> pEnum;
		if (SUCCEEDED(i_filter->EnumPins(&(pEnum.ptr))))
		{
			AutoReleaseComPtr<IPin> pPin;
			while (hr = pEnum->Next(1, &(pPin.ptr), 0), hr == S_OK)
			{
				PIN_DIRECTION ThisPinDir;
				hr = pPin->QueryDirection(&ThisPinDir);
				if (FAILED(hr))
				{
					throw NyWin32CaptureException();
				}
				GUID pin_category;
				mGetPinCategory(pPin, pin_category);

				if(Category==pin_category)
				{
					return pPin;
				}
				pPin.release();
			}
		}
		// 一致するピンがない。
		return NULL;
	}

	/*	グラフの持つ全てのフィルタピンを切断する。
	*/
	static void mDisconnectAll(IGraphBuilder* i_graph)
	{
		HRESULT hr = S_OK;
		if (i_graph==NULL)
		{
			throw NyWin32CaptureException();
		}

		AutoReleaseComPtr<IEnumFilters> enum_filter;
		hr = i_graph->EnumFilters(&(enum_filter.ptr));
		if(!SUCCEEDED(hr))
		{
			throw NyWin32CaptureException();
		}
		AutoReleaseComPtr<IBaseFilter> filter;
		while(S_OK == enum_filter->Next(1, &(filter.ptr), NULL))
		{
			AutoReleaseComPtr<IEnumPins> enum_pins;
			hr = filter->EnumPins(&(enum_pins.ptr));
			if(!SUCCEEDED(hr))
			{
				continue;
			}
			AutoReleaseComPtr<IPin> pin;
			while (S_OK == enum_pins->Next(1, &(pin.ptr), NULL))
			{
				AutoReleaseComPtr<IPin> pin2;
				if (S_OK == pin->ConnectedTo(&(pin2.ptr)))
				{
					// pins are connected, to disconnect filters, both pins must be disconnected
					hr = i_graph->Disconnect(pin);
					hr = i_graph->Disconnect(pin2);
				}
				pin.release();
			}
			filter.release();
		}
		return;
	}


	class CaptureImageCallback: public CUnknown, public ISampleGrabberCB
	{
	private:
		OnCaptureImage _callback;
	public:
		DECLARE_IUNKNOWN;

		STDMETHODIMP NonDelegatingQueryInterface(REFIID riid, void **ppv)
		{
			if( riid == IID_ISampleGrabberCB ){
				return GetInterface((ISampleGrabberCB*)this, ppv);
			}
			return CUnknown::NonDelegatingQueryInterface(riid, ppv);
		}
		STDMETHODIMP SampleCB(double SampleTime, IMediaSample *pSample)
		{
			return E_NOTIMPL;
		}

		STDMETHODIMP BufferCB(double SampleTime, BYTE *pBuffer, long BufferLen)
		{
			this->_callback(pBuffer,BufferLen);
			return S_OK;
		}
		// コンストラクタ
		CaptureImageCallback(OnCaptureImage i_callback) : CUnknown("SGCB", NULL)
		{
			this->_callback=i_callback;
		}

	};






}

//NyWin32CaptureException
//
namespace NyWin32Capture
{
	NyWin32CaptureException::NyWin32CaptureException():exception()
	{
	}
	NyWin32CaptureException::NyWin32CaptureException(exception e):exception(e)
	{
	}
	NyWin32CaptureException::NyWin32CaptureException(const char* m)
	{
		perror(m);
	}
}

//VideoFormat
//
namespace NyWin32Capture
{
	VideoFormat::VideoFormat(AM_MEDIA_TYPE* i_pmt,const VIDEO_STREAM_CONFIG_CAPS& i_scc)
	{
		this->_scc=(VIDEO_STREAM_CONFIG_CAPS*)malloc(sizeof(VIDEO_STREAM_CONFIG_CAPS));
		this->_pmt=i_pmt;
		*(this->_scc)=i_scc;
	}
	VideoFormat::~VideoFormat()
	{
		DeleteMediaType(this->_pmt);
		free(this->_scc);
	}
	int VideoFormat::getWidth()const
	{
		return reinterpret_cast<VIDEOINFOHEADER*>(this->_pmt->pbFormat)->bmiHeader.biWidth;
	}
	int VideoFormat::getHeight()const
	{
		return reinterpret_cast<VIDEOINFOHEADER*>(this->_pmt->pbFormat)->bmiHeader.biHeight;
	}
	double VideoFormat::getRate()const
	{
		return 10000000.0/((double)reinterpret_cast<VIDEOINFOHEADER*>(this->_pmt->pbFormat)->AvgTimePerFrame);
	}
	const GUID& VideoFormat::getFormat()const
	{
		return this->_pmt->subtype;
	}
	const AM_MEDIA_TYPE* VideoFormat::getMediaType()const
	{
		return reinterpret_cast<AM_MEDIA_TYPE*>(this->_pmt);
	}
	const VIDEOINFOHEADER* VideoFormat::getVideoInfoHeader()const
	{
		return reinterpret_cast<VIDEOINFOHEADER*>(this->_pmt->pbFormat);
	}
	const BITMAPINFOHEADER* VideoFormat::getBitmapInfoHeader()const
	{
		return &(reinterpret_cast<VIDEOINFOHEADER*>(this->_pmt->pbFormat)->bmiHeader);
	}
}

//VideoFormatList
//
namespace NyWin32Capture
{
	VideoFormatList::VideoFormatList()
	{
		this->_list=new std::vector<VideoFormat*>();
	}


	void VideoFormatList::clear()
	{
		for(int i=this->_list->size()-1;i>=0;i--){
			delete (*this->_list)[i];
		}
		this->_list->clear();
	}

	void VideoFormatList::update(IAMStreamConfig* i_config)
	{
		this->clear();
		//フォーマットの数を取得
		int iCount;
		int iSize;
		VIDEO_STREAM_CONFIG_CAPS scc;
		HRESULT hr;
		hr =i_config->GetNumberOfCapabilities( &iCount, &iSize );
		if(FAILED(hr) ||sizeof(scc) != iSize){
			throw NyWin32CaptureException();
		}
		for(int i=0;i<iCount;i++){
			AM_MEDIA_TYPE *pmt;
			hr = i_config->GetStreamCaps(i, &pmt,reinterpret_cast<BYTE*>(&scc));
			if(hr != S_OK)
			{
				DeleteMediaType(pmt);
				continue;
			}
			if (pmt->formattype != FORMAT_VideoInfo)
			{
				DeleteMediaType(pmt);
				continue;
			}
			VideoFormat* fmt=new VideoFormat(pmt,scc);
			//追加
			this->_list->push_back(fmt);
		}
		return;
	}
	VideoFormatList::~VideoFormatList()
	{
		this->clear();
		delete this->_list;
	}
	/**
	フォーマットキーに従って、フォーマットを検索する。
	*/
	const VideoFormat* VideoFormatList::getFormat(int i_width,int i_height,const GUID& i_format)const
	{
		//一致するそれっぽいのを探す。
		int l=(int)this->_list->size();
		for(int i=0;i<l;i++){
			const VideoFormat* f=((*this->_list)[i]);
			const VIDEOINFOHEADER* v=f->getVideoInfoHeader();
			if(i_width!=v->bmiHeader.biWidth)
			{
				continue;
			}
			if(i_height!=v->bmiHeader.biHeight)
			{
				continue;
			}
			if(i_format==f->getFormat())
			{
				continue;
			}
			return f;
		}
		return NULL;
	}
	const VideoFormat* VideoFormatList::getFormat(int i_index)const
	{
		return (*this->_list)[i_index];
	}
	int VideoFormatList::getNumberOfFormat()const
	{
		return (int)this->_list->size();
	}
}


namespace NyWin32Capture
{
	CaptureDevice::CaptureDevice(IMoniker* i_moniker)
	{
		//HRESULTチェックしてないところがある。
		HRESULT hr;
		hr=i_moniker-> BindToObject( 0, 0, IID_IBaseFilter, (void**)&this->ds_res.sorce.filter);
		this->_moniker =i_moniker;
		{	//nameの取得
			IPropertyBag* bag;
			hr=this->_moniker->BindToStorage(NULL,NULL,IID_IPropertyBag,(void**)&bag);
			if(!SUCCEEDED(hr)){
				throw NyWin32CaptureException();
			}
			VARIANT varName;
			varName.vt = VT_BSTR;
			hr=bag->Read(L"FriendlyName",&varName,NULL);
			this->_allocated_res.name=new WCHAR[wcslen(varName.bstrVal)+1];
			wcscpy(this->_allocated_res.name,varName.bstrVal);
			VariantClear(&varName);
			bag->Release();
		}
		this->_image_cb=NULL;
		this->_status  =ST_CLOSED;
	}
	CaptureDevice::~CaptureDevice()
	{
		switch(this->_status)
		{
		case ST_IDLE:
			this->closeDevice();
			break;
		case ST_RUN:
			this->stopCapture();
			this->closeDevice();
			break;
		default:
			break;
		}
		delete [] this->_allocated_res.name;
		this->_moniker->Release();
		this->ds_res.sorce.filter->Release();
	}

	void CaptureDevice::startCapture()
	{
		//状態チェック
		if(this->_status!=ST_IDLE){
			throw NyWin32CaptureException();
		}
		//HRESULTチェックしてないところがある。
		HRESULT hr;


		//キャプチャグラフを接続
		hr=this->ds_res.cap_builder->RenderStream(&this->ds_res.sorce.pin_category, &MEDIATYPE_Video,this->ds_res.sorce.filter, NULL,this->ds_res.render.filter);

		AM_MEDIA_TYPE amt;
		//現状のキャプチャ情報を取得
		hr=this->ds_res.render.grab->GetConnectedMediaType(&amt);
		CopyMemory(&this->_capture_format,&(amt.pbFormat),sizeof(VIDEOINFOHEADER));
		FreeMediaType(amt);

		//レンダラの開始
		hr=this->ds_res.render.grab->SetBufferSamples(TRUE);	// グラブ開始
		hr=this->ds_res.graph_builder.mc->Run();               // レンダリング開始

		this->_status=ST_RUN;
		return;
	}
	void CaptureDevice::startCaptureCallback(OnCaptureImage i_callback)
	{
		assert(i_callback!=NULL);
		//状態チェック
		if(this->_status!=ST_IDLE){
			throw NyWin32CaptureException();
		}
		HRESULT hr;
		try{
			//コールバックオブジェクト作る。
			//コールバックをセットする。
			this->_image_cb=new CaptureImageCallback(i_callback);
			hr=this->ds_res.render.grab->SetCallback(this->_image_cb,1);
		}catch(...){
			//失敗したときはコールバック関連のオブジェクトを削除
			this->ds_res.render.grab->SetCallback(NULL,1);
			this->_image_cb->Release();
			this->_image_cb=NULL;
		}
	}

	void CaptureDevice::stopCapture()
	{
		//状態チェック
		if(this->_status!=ST_RUN){
			throw NyWin32CaptureException();
		}
		//HRESULTチェックしてないところがある。
		HRESULT hr;

		//停止処理
		hr=this->ds_res.graph_builder.mc->Stop();
		hr=this->ds_res.render.grab->SetBufferSamples(FALSE);

		//コールバックが指定されていれば解除
		if(this->_image_cb!=NULL){
			this->ds_res.render.grab->SetCallback(NULL,1);
			this->_image_cb->Release();
			this->_image_cb=NULL;
		}
		//フィルタ内の全pinを切断
		mDisconnectAll(this->ds_res.graph_builder.graph);

		this->_status=ST_IDLE;
		return;
	}
	const VIDEOINFOHEADER& CaptureDevice::getVIDEOINFOHEADER()const
	{
		if(this->_status!=ST_RUN){
			throw NyWin32CaptureException();
		}
		return this->_capture_format;
	}
	const BITMAPINFOHEADER& CaptureDevice::getBITMAPINFOHEADER()const
	{
		if(this->_status!=ST_RUN){
			throw NyWin32CaptureException();
		}
		return this->_capture_format.bmiHeader;
	}

	/*	キャプチャデバイスをオープンします。
	*/
	void CaptureDevice::openDevice()
	{
		//状態チェック
		if(this->_status!=ST_CLOSED)
		{
			throw NyWin32CaptureException();
		}

		HRESULT hr;

		// グラバフィルタを作る
		AutoReleaseComPtr<IBaseFilter> pF;
		hr=CoCreateInstance(CLSID_SampleGrabber, NULL, CLSCTX_INPROC_SERVER,IID_IBaseFilter, (LPVOID *)&(pF.ptr));
		if(!SUCCEEDED(hr)){
			throw NyWin32CaptureException();
		}

		AutoReleaseComPtr<ISampleGrabber> pGrab;
		hr=pF -> QueryInterface( IID_ISampleGrabber, (void **)&(pGrab.ptr));
		if(!SUCCEEDED(hr)){
			throw NyWin32CaptureException();
		}

		//キャプチャグラフを作る  
		AutoReleaseComPtr<IGraphBuilder> pGraph;
		hr=CoCreateInstance( CLSID_FilterGraph, NULL, CLSCTX_INPROC,IID_IGraphBuilder, (void **) &(pGraph.ptr));
		if(!SUCCEEDED(hr)){
			throw NyWin32CaptureException();
		}

		AutoReleaseComPtr<IMediaControl> pMC;
		hr=pGraph -> QueryInterface( IID_IMediaControl, (LPVOID *) &(pMC.ptr));
		if(!SUCCEEDED(hr)){
			throw NyWin32CaptureException();
		}

		//CaptureGraphBuilderを作成
		AutoReleaseComPtr<ICaptureGraphBuilder2> pCapture;
		hr=CoCreateInstance( CLSID_CaptureGraphBuilder2 , NULL, CLSCTX_INPROC,IID_ICaptureGraphBuilder2, (void **) &(pCapture.ptr));
		if(!SUCCEEDED(hr)){
			throw NyWin32CaptureException();
		}

		// キャプチャフィルタをフィルタグラフに追加
		hr=pGraph -> AddFilter(this->ds_res.sorce.filter, L"CaptureFilter");
		if(!SUCCEEDED(hr)){
			throw NyWin32CaptureException();
		}

		//サンプルグラバの追加
		hr=pGraph -> AddFilter(pF, L"SampleGrabber");
		if(!SUCCEEDED(hr)){
			throw NyWin32CaptureException();
		}

		//フィルタグラフをキャプチャグラフに組み込む
		hr=pCapture -> SetFiltergraph(pGraph);
		if(!SUCCEEDED(hr)){
			throw NyWin32CaptureException();
		}

		//Configを得る(STILL→PREVIEWの順)
		AutoReleaseComPtr<IAMStreamConfig> config;
		if(mGetIAMStreamConfig(pCapture,this->ds_res.sorce.filter,PIN_CATEGORY_STILL,&config.ptr)){
			this->ds_res.sorce.pin_category=PIN_CATEGORY_STILL;
		}else{
			if(mGetIAMStreamConfig(pCapture,this->ds_res.sorce.filter,PIN_CATEGORY_PREVIEW,&config.ptr)){
				this->ds_res.sorce.pin_category=PIN_CATEGORY_PREVIEW;
			}else{
				throw NyWin32CaptureException();
			}
		}

		//自動開放ポインタから切り離す
		pCapture.detach(&this->ds_res.cap_builder);
		pF.detach(&this->ds_res.render.filter);
		pGrab.detach(&this->ds_res.render.grab);
		config.detach(&this->ds_res.sorce.config);
		pGraph.detach(&this->ds_res.graph_builder.graph);
		pMC.detach(&this->ds_res.graph_builder.mc);
		this->_status=ST_IDLE;
		return;
	}
	void CaptureDevice::closeDevice()
	{
		//状態チェック
		if(this->_status!=ST_IDLE){
			throw NyWin32CaptureException();
		}
		//インタフェイスのリリース
		this->ds_res.sorce.config->Release();
		this->ds_res.graph_builder.graph->Release();
		this->ds_res.graph_builder.mc->Release();
		this->ds_res.render.grab->Release();
		this->ds_res.render.filter->Release();
		this->ds_res.cap_builder->Release();
		this->_status=ST_CLOSED;
	}


	void CaptureDevice::getVideoFormatList(VideoFormatList& o_list)const
	{
		if(this->_status!=ST_IDLE){
			throw NyWin32CaptureException();
		}
		o_list.update(this->ds_res.sorce.config);
	}
	//i_bufには、this->_capture_format.biSizeImageより大きなサイズのバッファを与えること。
	bool CaptureDevice::captureImage(void* i_buf,long i_buf_size)
	{
		if(this->_status!=ST_RUN){
			throw NyWin32CaptureException();
		}

		HRESULT hr;
		long n=i_buf_size==0?this->_capture_format.bmiHeader.biSizeImage:i_buf_size;

		hr = this->ds_res.render.grab -> GetCurrentBuffer(&n,(long *)i_buf);
		return SUCCEEDED(hr);
	}
	bool CaptureDevice::setVideoFormat(int i_width,int i_height,GUID i_format,double i_rate)
	{
		if(this->_status!=ST_IDLE){
			throw NyWin32CaptureException();
		}
		VideoFormatList list;
		this->getVideoFormatList(list);
		const VideoFormat* vf=list.getFormat(i_width,i_height,i_format);
		return this->setVideoFormat(*vf,i_rate);
	}
	bool CaptureDevice::setVideoFormat(const VideoFormat& i_format,double i_rate)
	{
		if(this->_status!=ST_IDLE){
			throw NyWin32CaptureException();
		}
		//サンプルグラバの受け入れフォーマット設定
		HRESULT hr;
		hr=this->ds_res.render.grab->SetMediaType(i_format.getMediaType());
		if(!SUCCEEDED(hr)){
			return false;
		}

		//StreamConfigの設定
		AM_MEDIA_TYPE* nmt=CreateMediaType(i_format.getMediaType());
		reinterpret_cast<VIDEOINFOHEADER*>(nmt->pbFormat)->AvgTimePerFrame=(REFERENCE_TIME)(10000000.0/i_rate);
		hr=this->ds_res.sorce.config->SetFormat(nmt);
		if(!SUCCEEDED(hr)){
			DeleteMediaType(nmt);
			return false;
		}
		return true;
	}
	const WCHAR* CaptureDevice::getName()const
	{
		if(this->_status==ST_CLOSED){
			throw NyWin32CaptureException();
		}
		return this->_allocated_res.name;
	}
}

namespace NyWin32Capture
{
//		bool startCaptureCallback(ICaptureImageListener i_listener);

	void CaptureDeviceList::createDeviceList()
	{
		//リストの構築
		HRESULT hr;
		// ---- キャプチャフィルタの準備 ----
		// キャプチャデバイスを探す
		ULONG cFetched;
		// デバイス列挙子を作成
		AutoReleaseComPtr<ICreateDevEnum> pDevEnum;
		hr=CoCreateInstance(CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC,IID_ICreateDevEnum, (void ** ) &(pDevEnum.ptr));
		if(!SUCCEEDED(hr)){
			throw NyWin32CaptureException();
		}
		// ビデオキャプチャデバイス列挙子を作成
		AutoReleaseComPtr<IEnumMoniker> pClassEnum;
		hr=pDevEnum -> CreateClassEnumerator(CLSID_VideoInputDeviceCategory, &(pClassEnum.ptr), 0);
		if(hr!=S_OK){
			// do nothing
		}else{
			IMoniker * pMoniker = NULL;
			for(;;)
			{
				hr=pClassEnum -> Next(1, &pMoniker, &cFetched);
				if(hr!=S_OK){
					break;
				}
				CaptureDevice* cd=new CaptureDevice(pMoniker);
				this->_list->push_back(cd);
			}
		}
	}
	void CaptureDeviceList::releaseDeviceList()
	{
		for(unsigned int i=0;i<this->_list->size();i++){
			delete (*this->_list)[i];
		}
		(*this->_list).clear();
	}


	CaptureDeviceList::CaptureDeviceList()
	{
		this->_list=new vector<CaptureDevice*>;
		this->createDeviceList();
	}
	CaptureDeviceList::~CaptureDeviceList()
	{
		this->releaseDeviceList();
		delete this->_list;
	}
	//	i_index番目のキャプチャデバイスを取得します。
	CaptureDevice* CaptureDeviceList::getDevice(int i_index)const
	{
		return (*this->_list)[i_index];
	}
	//	デバイスの数を取得します。
	int CaptureDeviceList::getNumberOfDevice()const
	{
		return this->_list->size();
	}
};

