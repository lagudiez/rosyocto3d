/*********************************************************************
 *
 * $Id: yocto_api.cpp 16461 2014-06-06 14:44:21Z seb $
 *
 * High-level programming interface, common to all modules
 *
 * - - - - - - - - - License information: - - - - - - - - -
 *
 *  Copyright (C) 2011 and beyond by Yoctopuce Sarl, Switzerland.
 *
 *  Yoctopuce Sarl (hereafter Licensor) grants to you a perpetual
 *  non-exclusive license to use, modify, copy and integrate this
 *  file into your software for the sole purpose of interfacing 
 *  with Yoctopuce products. 
 *
 *  You may reproduce and distribute copies of this file in 
 *  source or object form, as long as the sole purpose of this
 *  code is to interface with Yoctopuce products. You must retain 
 *  this notice in the distributed source file.
 *
 *  You should refer to Yoctopuce General Terms and Conditions
 *  for additional information regarding your rights and 
 *  obligations.
 *
 *  THE SOFTWARE AND DOCUMENTATION ARE PROVIDED "AS IS" WITHOUT
 *  WARRANTY OF ANY KIND, EITHER EXPRESS OR IMPLIED, INCLUDING 
 *  WITHOUT LIMITATION, ANY WARRANTY OF MERCHANTABILITY, FITNESS 
 *  FOR A PARTICULAR PURPOSE, TITLE AND NON-INFRINGEMENT. IN NO
 *  EVENT SHALL LICENSOR BE LIABLE FOR ANY INCIDENTAL, SPECIAL,
 *  INDIRECT OR CONSEQUENTIAL DAMAGES, LOST PROFITS OR LOST DATA, 
 *  COST OF PROCUREMENT OF SUBSTITUTE GOODS, TECHNOLOGY OR 
 *  SERVICES, ANY CLAIMS BY THIRD PARTIES (INCLUDING BUT NOT 
 *  LIMITED TO ANY DEFENSE THEREOF), ANY CLAIMS FOR INDEMNITY OR
 *  CONTRIBUTION, OR OTHER SIMILAR COSTS, WHETHER ASSERTED ON THE
 *  BASIS OF CONTRACT, TORT (INCLUDING NEGLIGENCE), BREACH OF
 *  WARRANTY, OR OTHERWISE.
 *
 *********************************************************************/
#define __FILE_ID__ "yocto_api"
#define _CRT_SECURE_NO_DEPRECATE
#include "yocto3d/yocto_api.h"
#include "yocto3d/yapi/yapi.h"

#ifdef WINDOWS_API
#include <Windows.h>
#define yySleep(ms)          Sleep(ms)
#else
#include <unistd.h>
#define yySleep(ms)          usleep(ms*1000)
#endif

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <cfloat>
#include <cmath>
#include <time.h>
#include <stdarg.h>
#include <math.h>

static  yCRITICAL_SECTION   _updateDeviceList_CS;
static  yCRITICAL_SECTION   _handleEvent_CS;

static  std::vector<YFunction*>     _FunctionCache;
static  std::vector<YFunction*>     _FunctionCallbacks;
static  std::vector<YFunction*>     _TimedReportCallbackList;


const string YFunction::HARDWAREID_INVALID = YAPI_INVALID_STRING;
const string YFunction::FUNCTIONID_INVALID = YAPI_INVALID_STRING;
const string YFunction::FRIENDLYNAME_INVALID = YAPI_INVALID_STRING;

const double YDataStream::DATA_INVALID = Y_DATA_INVALID;

int _ystrpos(const string& haystack, const string& needle)
{
    size_t  pos = haystack.find(needle);
    if(pos == string::npos) {
        return -1;
    }
    return (int)pos;
}

// YDataStream constructor for the new datalogger
YDataStream::YDataStream(YFunction *parent, YDataSet& dataset, const vector<int>& encoded)
{
    _parent   = parent;
    this->_initFromDataSet(&dataset, encoded);
}

// YDataSet constructor, when instantiated directly by a function
YDataSet::YDataSet(YFunction *parent, const string& functionId, const string& unit, s64 startTime, s64 endTime)
{
    _parent     = parent;
    _functionId = functionId;
    _unit       = unit;
    _startTime  = startTime;
    _endTime    = endTime;
    _summary = YMeasure(0, 0, 0, 0, 0);
    _progress   = -1;
}

// YDataSet constructor for the new datalogger
YDataSet::YDataSet(YFunction *parent, const string& json)
{
    _parent    = parent;
    _startTime = 0;
    _endTime   = 0;
    _summary = YMeasure(0, 0, 0, 0, 0);
    this->_parse(json);
}

// YDataSet parser for stream list
int YDataSet::_parse(const string& json)
{
    yJsonStateMachine j;
    double summaryMinVal=DBL_MAX;
    double summaryMaxVal=-DBL_MAX;
    double summaryTotalTime=0;
    double summaryTotalAvg=0;

    
    // Parse JSON data
    j.src = json.c_str();
    j.end = j.src + strlen(j.src);
    j.st = YJSON_START;
    if(yJsonParse(&j) != YJSON_PARSE_AVAIL || j.st != YJSON_PARSE_STRUCT) {
        return YAPI_NOT_SUPPORTED;
    }
    while(yJsonParse(&j) == YJSON_PARSE_AVAIL && j.st == YJSON_PARSE_MEMBNAME) {
        if (!strcmp(j.token, "id")) {
            if (yJsonParse(&j) != YJSON_PARSE_AVAIL) {
                return YAPI_NOT_SUPPORTED;
            }
            _functionId = _parent->_parseString(j);
        } else if (!strcmp(j.token, "unit")) {
            if (yJsonParse(&j) != YJSON_PARSE_AVAIL) {
                return YAPI_NOT_SUPPORTED;
            }
            _unit = _parent->_parseString(j);
        } else if (!strcmp(j.token, "cal")) {
            if (yJsonParse(&j) != YJSON_PARSE_AVAIL) {
                return YAPI_NOT_SUPPORTED;
            }
            _calib = YAPI::_decodeWords(_parent->_parseString(j));
        } else if (!strcmp(j.token, "streams")) {
            YDataStream *stream;
            _streams = vector<YDataStream*>();
            _preview = vector<YMeasure>();
            _measures = vector<YMeasure>();
            if (yJsonParse(&j) != YJSON_PARSE_AVAIL || j.token[0] != '[') {
                return YAPI_NOT_SUPPORTED;
            }
            // select streams for specified timeframe
            while(yJsonParse(&j) == YJSON_PARSE_AVAIL && j.token[0] != ']') {
                stream = _parent->_findDataStream(*this,_parent->_parseString(j));
                if(_startTime > 0 && stream->get_startTimeUTC() + stream->get_duration() <= _startTime) {
                    // this stream is too early, drop it
                } else if(_endTime > 0 && stream->get_startTimeUTC() > _endTime) {
                    // this stream is too late, drop it
                } else {
                    _streams.push_back(stream);
                    if(stream->isClosed() && stream->get_startTimeUTC() >= _startTime &&
                       (_endTime == 0 || stream->get_startTimeUTC() + stream->get_duration() <= _endTime)) {
                        if (summaryMinVal > stream->get_minValue())
                            summaryMinVal =stream->get_minValue();
                        if (summaryMaxVal < stream->get_maxValue())
                            summaryMaxVal =stream->get_maxValue();
                        summaryTotalAvg  += stream->get_averageValue() * stream->get_duration();
                        summaryTotalTime += stream->get_duration();

                        YMeasure rec = YMeasure((double)stream->get_startTimeUTC(),
                                                (double)(stream->get_startTimeUTC() + stream->get_duration()),
                                                stream->get_minValue(),
                                                stream->get_averageValue(),
                                                stream->get_maxValue());
                        _preview.push_back(rec);
                    }
                }
            }
            if((_streams.size() > 0)  && (summaryTotalTime>0)) {
                // update time boundaries with actual data
                stream = _streams[_streams.size()-1];
                s64 endtime = stream->get_startTimeUTC() + stream->get_duration();
                s64 startTime = _streams[0]->get_startTimeUTC() - stream->get_dataSamplesIntervalMs()/1000;
                if(_startTime < startTime) {
                    _startTime = startTime;
                }
                if(_endTime == 0 || _endTime > endtime) {
                    _endTime = endtime;
                }
                _summary = YMeasure((double)_startTime,(double)_endTime,summaryMinVal,summaryTotalAvg/summaryTotalTime,summaryMaxVal);
            }
        } else {
            yJsonSkip(&j, 1);
        }
    }
    _progress = 0;
    return this->get_progress();
}



//--- (generated code: YDataStream implementation)
// static attributes


int YDataStream::_initFromDataSet(YDataSet* dataset,vector<int> encoded)
{
    int val = 0;
    int i = 0;
    int iRaw = 0;
    int iRef = 0;
    double fRaw = 0.0;
    double fRef = 0.0;
    double duration_float = 0.0;
    vector<int> iCalib;
    
    // decode sequence header to extract data
    _runNo = encoded[0] + (((encoded[1]) << (16)));
    _utcStamp = encoded[2] + (((encoded[3]) << (16)));
    val = encoded[4];
    _isAvg = (((val) & (0x100)) == 0);
    _samplesPerHour = ((val) & (0xff));
    if (((val) & (0x100)) != 0) {
        _samplesPerHour = _samplesPerHour * 3600;
    } else {
        if (((val) & (0x200)) != 0) {
            _samplesPerHour = _samplesPerHour * 60;
        }
    }
    
    val = encoded[5];
    if (val > 32767) {
        val = val - 65536;
    }
    _decimals = val;
    _offset = val;
    _scale = encoded[6];
    _isScal = (_scale != 0);
    
    val = encoded[7];
    _isClosed = (val != 0xffff);
    if (val == 0xffff) {
        val = 0;
    }
    _nRows = val;
    duration_float = _nRows * 3600 / _samplesPerHour;
    _duration = (int) (duration_float < 0.0 ? ceil(duration_float-0.5) : floor(duration_float+0.5));
    // precompute decoding parameters
    _decexp = 1.0;
    if (_scale == 0) {
        i = 0;
        while (i < _decimals) {
            _decexp = _decexp * 10.0;
            i = i + 1;
        }
    }
    iCalib = dataset->get_calibration();
    _caltyp = iCalib[0];
    if (_caltyp != 0) {
        _calhdl = YAPI::_getCalibrationHandler(_caltyp);
        _calpar.clear();
        _calraw.clear();
        _calref.clear();
        i = 1;
        while (i + 1 < (int)iCalib.size()) {
            iRaw = iCalib[i];
            iRef = iCalib[i + 1];
            _calpar.push_back(iRaw);
            _calpar.push_back(iRef);
            if (_isScal) {
                fRaw = iRaw;
                fRaw = (fRaw - _offset) / _scale;
                fRef = iRef;
                fRef = (fRef - _offset) / _scale;
                _calraw.push_back(fRaw);
                _calref.push_back(fRef);
            } else {
                _calraw.push_back(YAPI::_decimalToDouble(iRaw));
                _calref.push_back(YAPI::_decimalToDouble(iRef));
            }
            i = i + 2;
        }
    }
    // preload column names for backward-compatibility
    _functionId = dataset->get_functionId();
    if (_isAvg) {
        _columnNames.clear();
        _columnNames.push_back(YapiWrapper::ysprintf("%s_min",_functionId.c_str()));
        _columnNames.push_back(YapiWrapper::ysprintf("%s_avg",_functionId.c_str()));
        _columnNames.push_back(YapiWrapper::ysprintf("%s_max",_functionId.c_str()));
        _nCols = 3;
    } else {
        _columnNames.clear();
        _columnNames.push_back(_functionId);
        _nCols = 1;
    }
    // decode min/avg/max values for the sequence
    if (_nRows > 0) {
        _minVal = this->_decodeVal(encoded[8]);
        _maxVal = this->_decodeVal(encoded[9]);
        _avgVal = this->_decodeAvg(encoded[10] + (((encoded[11]) << (16))), _nRows);
    }
    return 0;
}

int YDataStream::parse(string sdata)
{
    int idx = 0;
    vector<int> udat;
    vector<double> dat;
    // may throw an exception
    udat = YAPI::_decodeWords(_parent->_json_get_string(sdata));
    _values.clear();
    idx = 0;
    if (_isAvg) {
        while (idx + 3 < (int)udat.size()) {
            dat.clear();
            dat.push_back(this->_decodeVal(udat[idx]));
            dat.push_back(this->_decodeAvg(udat[idx + 2] + (((udat[idx + 3]) << (16))), 1));
            dat.push_back(this->_decodeVal(udat[idx + 1]));
            _values.push_back(dat);
            idx = idx + 4;
        }
    } else {
        if (_isScal) {
            while (idx < (int)udat.size()) {
                dat.clear();
                dat.push_back(this->_decodeVal(udat[idx]));
                _values.push_back(dat);
                idx = idx + 1;
            }
        } else {
            while (idx + 1 < (int)udat.size()) {
                dat.clear();
                dat.push_back(this->_decodeAvg(udat[idx] + (((udat[idx + 1]) << (16))), 1));
                _values.push_back(dat);
                idx = idx + 2;
            }
        }
    }
    
    _nRows = (int)_values.size();
    return YAPI_SUCCESS;
}

string YDataStream::get_url(void)
{
    string url;
    url = YapiWrapper::ysprintf("logger.json?id=%s&run=%d&utc=%u",
    _functionId.c_str(),_runNo,_utcStamp);
    return url;
}

int YDataStream::loadStream(void)
{
    return this->parse(_parent->_download(this->get_url()));
}

double YDataStream::_decodeVal(int w)
{
    double val = 0.0;
    val = w;
    if (_isScal) {
        val = (val - _offset) / _scale;
    } else {
        val = YAPI::_decimalToDouble(w);
    }
    if (_caltyp != 0) {
        val = _calhdl(val, _caltyp, _calpar, _calraw, _calref);
    }
    return val;
}

double YDataStream::_decodeAvg(int dw,int count)
{
    double val = 0.0;
    val = dw;
    if (_isScal) {
        val = (val / (100 * count) - _offset) / _scale;
    } else {
        val = val / (count * _decexp);
    }
    if (_caltyp != 0) {
        val = _calhdl(val, _caltyp, _calpar, _calraw, _calref);
    }
    return val;
}

bool YDataStream::isClosed(void)
{
    return _isClosed;
}

/**
 * Returns the run index of the data stream. A run can be made of
 * multiple datastreams, for different time intervals.
 * 
 * @return an unsigned number corresponding to the run index.
 */
int YDataStream::get_runIndex(void)
{
    return _runNo;
}

/**
 * Returns the relative start time of the data stream, measured in seconds.
 * For recent firmwares, the value is relative to the present time,
 * which means the value is always negative.
 * If the device uses a firmware older than version 13000, value is
 * relative to the start of the time the device was powered on, and
 * is always positive.
 * If you need an absolute UTC timestamp, use get_startTimeUTC().
 * 
 * @return an unsigned number corresponding to the number of seconds
 *         between the start of the run and the beginning of this data
 *         stream.
 */
int YDataStream::get_startTime(void)
{
    return (int)(_utcStamp - ((unsigned)time(NULL)));
}

/**
 * Returns the start time of the data stream, relative to the Jan 1, 1970.
 * If the UTC time was not set in the datalogger at the time of the recording
 * of this data stream, this method returns 0.
 * 
 * @return an unsigned number corresponding to the number of seconds
 *         between the Jan 1, 1970 and the beginning of this data
 *         stream (i.e. Unix time representation of the absolute time).
 */
s64 YDataStream::get_startTimeUTC(void)
{
    return _utcStamp;
}

/**
 * Returns the number of milliseconds between two consecutive
 * rows of this data stream. By default, the data logger records one row
 * per second, but the recording frequency can be changed for
 * each device function
 * 
 * @return an unsigned number corresponding to a number of milliseconds.
 */
int YDataStream::get_dataSamplesIntervalMs(void)
{
    return ((3600000) / (_samplesPerHour));
}

double YDataStream::get_dataSamplesInterval(void)
{
    return 3600.0 / _samplesPerHour;
}

/**
 * Returns the number of data rows present in this stream.
 * 
 * If the device uses a firmware older than version 13000,
 * this method fetches the whole data stream from the device
 * if not yet done, which can cause a little delay.
 * 
 * @return an unsigned number corresponding to the number of rows.
 * 
 * On failure, throws an exception or returns zero.
 */
int YDataStream::get_rowCount(void)
{
    if ((_nRows != 0) && _isClosed) {
        return _nRows;
    }
    this->loadStream();
    return _nRows;
}

/**
 * Returns the number of data columns present in this stream.
 * The meaning of the values present in each column can be obtained
 * using the method get_columnNames().
 * 
 * If the device uses a firmware older than version 13000,
 * this method fetches the whole data stream from the device
 * if not yet done, which can cause a little delay.
 * 
 * @return an unsigned number corresponding to the number of columns.
 * 
 * On failure, throws an exception or returns zero.
 */
int YDataStream::get_columnCount(void)
{
    if (_nCols != 0) {
        return _nCols;
    }
    this->loadStream();
    return _nCols;
}

/**
 * Returns the title (or meaning) of each data column present in this stream.
 * In most case, the title of the data column is the hardware identifier
 * of the sensor that produced the data. For streams recorded at a lower
 * recording rate, the dataLogger stores the min, average and max value
 * during each measure interval into three columns with suffixes _min,
 * _avg and _max respectively.
 * 
 * If the device uses a firmware older than version 13000,
 * this method fetches the whole data stream from the device
 * if not yet done, which can cause a little delay.
 * 
 * @return a list containing as many strings as there are columns in the
 *         data stream.
 * 
 * On failure, throws an exception or returns an empty array.
 */
vector<string> YDataStream::get_columnNames(void)
{
    if ((int)_columnNames.size() != 0) {
        return _columnNames;
    }
    this->loadStream();
    return _columnNames;
}

/**
 * Returns the smallest measure observed within this stream.
 * If the device uses a firmware older than version 13000,
 * this method will always return Y_DATA_INVALID.
 * 
 * @return a floating-point number corresponding to the smallest value,
 *         or Y_DATA_INVALID if the stream is not yet complete (still recording).
 * 
 * On failure, throws an exception or returns Y_DATA_INVALID.
 */
double YDataStream::get_minValue(void)
{
    return _minVal;
}

/**
 * Returns the average of all measures observed within this stream.
 * If the device uses a firmware older than version 13000,
 * this method will always return Y_DATA_INVALID.
 * 
 * @return a floating-point number corresponding to the average value,
 *         or Y_DATA_INVALID if the stream is not yet complete (still recording).
 * 
 * On failure, throws an exception or returns Y_DATA_INVALID.
 */
double YDataStream::get_averageValue(void)
{
    return _avgVal;
}

/**
 * Returns the largest measure observed within this stream.
 * If the device uses a firmware older than version 13000,
 * this method will always return Y_DATA_INVALID.
 * 
 * @return a floating-point number corresponding to the largest value,
 *         or Y_DATA_INVALID if the stream is not yet complete (still recording).
 * 
 * On failure, throws an exception or returns Y_DATA_INVALID.
 */
double YDataStream::get_maxValue(void)
{
    return _maxVal;
}

/**
 * Returns the approximate duration of this stream, in seconds.
 * 
 * @return the number of seconds covered by this stream.
 * 
 * On failure, throws an exception or returns Y_DURATION_INVALID.
 */
int YDataStream::get_duration(void)
{
    if (_isClosed) {
        return _duration;
    }
    return (int)(((unsigned)time(NULL)) - _utcStamp);
}

/**
 * Returns the whole data set contained in the stream, as a bidimensional
 * table of numbers.
 * The meaning of the values present in each column can be obtained
 * using the method get_columnNames().
 * 
 * This method fetches the whole data stream from the device,
 * if not yet done.
 * 
 * @return a list containing as many elements as there are rows in the
 *         data stream. Each row itself is a list of floating-point
 *         numbers.
 * 
 * On failure, throws an exception or returns an empty array.
 */
vector< vector<double> > YDataStream::get_dataRows(void)
{
    if (((int)_values.size() == 0) || !(_isClosed)) {
        this->loadStream();
    }
    return _values;
}

/**
 * Returns a single measure from the data stream, specified by its
 * row and column index.
 * The meaning of the values present in each column can be obtained
 * using the method get_columnNames().
 * 
 * This method fetches the whole data stream from the device,
 * if not yet done.
 * 
 * @param row : row index
 * @param col : column index
 * 
 * @return a floating-point number
 * 
 * On failure, throws an exception or returns Y_DATA_INVALID.
 */
double YDataStream::get_data(int row,int col)
{
    if (((int)_values.size() == 0) || !(_isClosed)) {
        this->loadStream();
    }
    if (row >= (int)_values.size()) {
        return Y_DATA_INVALID;
    }
    if (col >= (int)_values[row].size()) {
        return Y_DATA_INVALID;
    }
    return _values[row][col];
}
//--- (end of generated code: YDataStream implementation)


//--- (generated code: YMeasure implementation)
// static attributes


/**
 * Returns the start time of the measure, relative to the Jan 1, 1970 UTC
 * (Unix timestamp). When the recording rate is higher then 1 sample
 * per second, the timestamp may have a fractional part.
 * 
 * @return an floating point number corresponding to the number of seconds
 *         between the Jan 1, 1970 UTC and the beginning of this measure.
 */
double YMeasure::get_startTimeUTC(void)
{
    return _start;
}

/**
 * Returns the end time of the measure, relative to the Jan 1, 1970 UTC
 * (Unix timestamp). When the recording rate is higher than 1 sample
 * per second, the timestamp may have a fractional part.
 * 
 * @return an floating point number corresponding to the number of seconds
 *         between the Jan 1, 1970 UTC and the end of this measure.
 */
double YMeasure::get_endTimeUTC(void)
{
    return _end;
}

/**
 * Returns the smallest value observed during the time interval
 * covered by this measure.
 * 
 * @return a floating-point number corresponding to the smallest value observed.
 */
double YMeasure::get_minValue(void)
{
    return _minVal;
}

/**
 * Returns the average value observed during the time interval
 * covered by this measure.
 * 
 * @return a floating-point number corresponding to the average value observed.
 */
double YMeasure::get_averageValue(void)
{
    return _avgVal;
}

/**
 * Returns the largest value observed during the time interval
 * covered by this measure.
 * 
 * @return a floating-point number corresponding to the largest value observed.
 */
double YMeasure::get_maxValue(void)
{
    return _maxVal;
}
//--- (end of generated code: YMeasure implementation)

time_t*   YMeasure::get_startTimeUTC_asTime_t(time_t *time)
{
    if(time){
        memcpy(time,&this->_stopTime_t,sizeof(time_t));
    }
    return &this->_startTime_t;
    
}
time_t*   YMeasure::get_endTimeUTC_asTime_t(time_t *time)
{
    if(time){
        memcpy(time,&this->_stopTime_t,sizeof(time_t));
    }        
    return &this->_stopTime_t;
}

//--- (generated code: YDataSet implementation)
// static attributes


vector<int> YDataSet::get_calibration(void)
{
    return _calib;
}

int YDataSet::processMore(int progress,string data)
{
    YDataStream* stream = NULL;
    vector< vector<double> > dataRows;
    string strdata;
    double tim = 0.0;
    double itv = 0.0;
    int nCols = 0;
    int minCol = 0;
    int avgCol = 0;
    int maxCol = 0;
    // may throw an exception
    if (progress != _progress) {
        return _progress;
    }
    if (_progress < 0) {
        strdata = data;
        if (strdata == "{}") {
            _parent->_throw(YAPI_VERSION_MISMATCH, "device firmware is too old");
            return YAPI_VERSION_MISMATCH;
        }
        return this->_parse(strdata);
    }
    stream = _streams[_progress];
    stream->parse(data);
    dataRows = stream->get_dataRows();
    _progress = _progress + 1;
    if ((int)dataRows.size() == 0) {
        return this->get_progress();
    }
    tim = (double) stream->get_startTimeUTC();
    itv = stream->get_dataSamplesInterval();
    nCols = (int)dataRows[0].size();
    minCol = 0;
    if (nCols > 2) {
        avgCol = 1;
    } else {
        avgCol = 0;
    }
    if (nCols > 2) {
        maxCol = 2;
    } else {
        maxCol = 0;
    }
    
    for (unsigned ii = 0; ii < dataRows.size(); ii++) {
        if ((tim >= _startTime) && ((_endTime == 0) || (tim <= _endTime))) {
            _measures.push_back(YMeasure(tim - itv, tim,
            dataRows[ii][minCol],
            dataRows[ii][avgCol],dataRows[ii][maxCol]));
            tim = tim + itv;
        }
    }
    
    return this->get_progress();
}

vector<YDataStream*> YDataSet::get_privateDataStreams(void)
{
    return _streams;
}

/**
 * Returns the unique hardware identifier of the function who performed the measures,
 * in the form SERIAL.FUNCTIONID. The unique hardware identifier is composed of the
 * device serial number and of the hardware identifier of the function
 * (for example THRMCPL1-123456.temperature1)
 * 
 * @return a string that uniquely identifies the function (ex: THRMCPL1-123456.temperature1)
 * 
 * On failure, throws an exception or returns  Y_HARDWAREID_INVALID.
 */
string YDataSet::get_hardwareId(void)
{
    YModule* mo = NULL;
    if (!(_hardwareId == "")) {
        return _hardwareId;
    }
    mo = _parent->get_module();
    _hardwareId = YapiWrapper::ysprintf("%s.%s", mo->get_serialNumber().c_str(),this->get_functionId().c_str());
    return _hardwareId;
}

/**
 * Returns the hardware identifier of the function that performed the measure,
 * without reference to the module. For example temperature1.
 * 
 * @return a string that identifies the function (ex: temperature1)
 */
string YDataSet::get_functionId(void)
{
    return _functionId;
}

/**
 * Returns the measuring unit for the measured value.
 * 
 * @return a string that represents a physical unit.
 * 
 * On failure, throws an exception or returns  Y_UNIT_INVALID.
 */
string YDataSet::get_unit(void)
{
    return _unit;
}

/**
 * Returns the start time of the dataset, relative to the Jan 1, 1970.
 * When the YDataSet is created, the start time is the value passed
 * in parameter to the get_dataSet() function. After the
 * very first call to loadMore(), the start time is updated
 * to reflect the timestamp of the first measure actually found in the
 * dataLogger within the specified range.
 * 
 * @return an unsigned number corresponding to the number of seconds
 *         between the Jan 1, 1970 and the beginning of this data
 *         set (i.e. Unix time representation of the absolute time).
 */
s64 YDataSet::get_startTimeUTC(void)
{
    return _startTime;
}

/**
 * Returns the end time of the dataset, relative to the Jan 1, 1970.
 * When the YDataSet is created, the end time is the value passed
 * in parameter to the get_dataSet() function. After the
 * very first call to loadMore(), the end time is updated
 * to reflect the timestamp of the last measure actually found in the
 * dataLogger within the specified range.
 * 
 * @return an unsigned number corresponding to the number of seconds
 *         between the Jan 1, 1970 and the end of this data
 *         set (i.e. Unix time representation of the absolute time).
 */
s64 YDataSet::get_endTimeUTC(void)
{
    return _endTime;
}

/**
 * Returns the progress of the downloads of the measures from the data logger,
 * on a scale from 0 to 100. When the object is instanciated by get_dataSet,
 * the progress is zero. Each time loadMore() is invoked, the progress
 * is updated, to reach the value 100 only once all measures have been loaded.
 * 
 * @return an integer in the range 0 to 100 (percentage of completion).
 */
int YDataSet::get_progress(void)
{
    if (_progress < 0) {
        return 0;
    }
    // index not yet loaded
    if (_progress >= (int)_streams.size()) {
        return 100;
    }
    return ((1 + (1 + _progress) * 98) / ((1 + (int)_streams.size())));
}

/**
 * Loads the the next block of measures from the dataLogger, and updates
 * the progress indicator.
 * 
 * @return an integer in the range 0 to 100 (percentage of completion),
 *         or a negative error code in case of failure.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
int YDataSet::loadMore(void)
{
    string url;
    YDataStream* stream = NULL;
    if (_progress < 0) {
        url = YapiWrapper::ysprintf("logger.json?id=%s",_functionId.c_str());
    } else {
        if (_progress >= (int)_streams.size()) {
            return 100;
        } else {
            stream = _streams[_progress];
            url = stream->get_url();
        }
    }
    return this->processMore(_progress, _parent->_download(url));
}

/**
 * Returns an YMeasure object which summarizes the whole
 * DataSet. In includes the following information:
 * - the start of a time interval
 * - the end of a time interval
 * - the minimal value observed during the time interval
 * - the average value observed during the time interval
 * - the maximal value observed during the time interval
 * 
 * This summary is available as soon as loadMore() has
 * been called for the first time.
 * 
 * @return an YMeasure object
 */
YMeasure YDataSet::get_summary(void)
{
    return _summary;
}

/**
 * Returns a condensed version of the measures that can
 * retrieved in this YDataSet, as a list of YMeasure
 * objects. Each item includes:
 * - the start of a time interval
 * - the end of a time interval
 * - the minimal value observed during the time interval
 * - the average value observed during the time interval
 * - the maximal value observed during the time interval
 * 
 * This preview is available as soon as loadMore() has
 * been called for the first time.
 * 
 * @return a table of records, where each record depicts the
 *         measured values during a time interval
 * 
 * On failure, throws an exception or returns an empty array.
 */
vector<YMeasure> YDataSet::get_preview(void)
{
    return _preview;
}

/**
 * Returns all measured values currently available for this DataSet,
 * as a list of YMeasure objects. Each item includes:
 * - the start of the measure time interval
 * - the end of the measure time interval
 * - the minimal value observed during the time interval
 * - the average value observed during the time interval
 * - the maximal value observed during the time interval
 * 
 * Before calling this method, you should call loadMore()
 * to load data from the device. You may have to call loadMore()
 * several time until all rows are loaded, but you can start
 * looking at available data rows before the load is complete.
 * 
 * The oldest measures are always loaded first, and the most
 * recent measures will be loaded last. As a result, timestamps
 * are normally sorted in ascending order within the measure table,
 * unless there was an unexpected adjustment of the datalogger UTC
 * clock.
 * 
 * @return a table of records, where each record depicts the
 *         measured value for a given time interval
 * 
 * On failure, throws an exception or returns an empty array.
 */
vector<YMeasure> YDataSet::get_measures(void)
{
    return _measures;
}
//--- (end of generated code: YDataSet implementation)


std::map<string,YFunction*> YFunction::_cache;


// Constructor is protected. Use the device-specific factory function to instantiate
YFunction::YFunction(const string& func):
    _className("Function"),_func(func),
    _lastErrorType(YAPI_SUCCESS),_lastErrorMsg(""),
    _fundescr(Y_FUNCTIONDESCRIPTOR_INVALID), _userData(NULL)
//--- (generated code: Function initialization)
    ,_logicalName(LOGICALNAME_INVALID)
    ,_advertisedValue(ADVERTISEDVALUE_INVALID)
    ,_valueCallbackFunction(NULL)
    ,_cacheExpiration(0)
//--- (end of generated code: Function initialization)
{
     _FunctionCache.push_back(this);
}

YFunction::~YFunction() 
{
//--- (generated code: Function cleanup)
//--- (end of generated code: Function cleanup)
}


// function cache methods
YFunction*  YFunction::_FindFromCache(const string& classname, const string& func)
{
     if(_cache.find(classname + "_" + func) != _cache.end())
        return _cache[classname + "_" + func];
     return NULL;
}

void        YFunction::_AddToCache(const string& classname, const string& func, YFunction *obj)
{
    _cache[classname + "_" + func] = obj;
}

void YFunction::_ClearCache()
{
    for (std::map<string, YFunction*>::iterator cache_iterator = _cache.begin();
             cache_iterator != _cache.end(); ++cache_iterator){
        delete cache_iterator->second;
    }
    _cache.clear();
}



//--- (generated code: YFunction implementation)
// static attributes
const string YFunction::LOGICALNAME_INVALID = YAPI_INVALID_STRING;
const string YFunction::ADVERTISEDVALUE_INVALID = YAPI_INVALID_STRING;

int YFunction::_parseAttr(yJsonStateMachine& j)
{
    if(!strcmp(j.token, "logicalName")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _logicalName =  _parseString(j);
        return 1;
    }
    if(!strcmp(j.token, "advertisedValue")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _advertisedValue =  _parseString(j);
        return 1;
    }
    failed:
    return 0;
}


/**
 * Returns the logical name of the function.
 * 
 * @return a string corresponding to the logical name of the function
 * 
 * On failure, throws an exception or returns Y_LOGICALNAME_INVALID.
 */
string YFunction::get_logicalName(void)
{
    if (_cacheExpiration <= YAPI::GetTickCount()) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YFunction::LOGICALNAME_INVALID;
        }
    }
    return _logicalName;
}

/**
 * Changes the logical name of the function. You can use yCheckLogicalName()
 * prior to this call to make sure that your parameter is valid.
 * Remember to call the saveToFlash() method of the module if the
 * modification must be kept.
 * 
 * @param newval : a string corresponding to the logical name of the function
 * 
 * @return YAPI_SUCCESS if the call succeeds.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
int YFunction::set_logicalName(const string& newval)
{
    string rest_val;
    if (!YAPI::CheckLogicalName(newval)) {
        _throw(YAPI_INVALID_ARGUMENT, "Invalid name :" + newval);
        return YAPI_INVALID_ARGUMENT;
    }
    rest_val = newval;
    return _setAttr("logicalName", rest_val);
}

/**
 * Returns the current value of the function (no more than 6 characters).
 * 
 * @return a string corresponding to the current value of the function (no more than 6 characters)
 * 
 * On failure, throws an exception or returns Y_ADVERTISEDVALUE_INVALID.
 */
string YFunction::get_advertisedValue(void)
{
    if (_cacheExpiration <= YAPI::GetTickCount()) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YFunction::ADVERTISEDVALUE_INVALID;
        }
    }
    return _advertisedValue;
}

/**
 * Retrieves a function for a given identifier.
 * The identifier can be specified using several formats:
 * <ul>
 * <li>FunctionLogicalName</li>
 * <li>ModuleSerialNumber.FunctionIdentifier</li>
 * <li>ModuleSerialNumber.FunctionLogicalName</li>
 * <li>ModuleLogicalName.FunctionIdentifier</li>
 * <li>ModuleLogicalName.FunctionLogicalName</li>
 * </ul>
 * 
 * This function does not require that the function is online at the time
 * it is invoked. The returned object is nevertheless valid.
 * Use the method YFunction.isOnline() to test if the function is
 * indeed online at a given time. In case of ambiguity when looking for
 * a function by logical name, no error is notified: the first instance
 * found is returned. The search is performed first by hardware name,
 * then by logical name.
 * 
 * @param func : a string that uniquely characterizes the function
 * 
 * @return a YFunction object allowing you to drive the function.
 */
YFunction* YFunction::FindFunction(string func)
{
    YFunction* obj = NULL;
    obj = (YFunction*) YFunction::_FindFromCache("Function", func);
    if (obj == NULL) {
        obj = new YFunction(func);
        YFunction::_AddToCache("Function", func, obj);
    }
    return obj;
}

/**
 * Registers the callback function that is invoked on every change of advertised value.
 * The callback is invoked only during the execution of ySleep or yHandleEvents.
 * This provides control over the time when the callback is triggered. For good responsiveness, remember to call
 * one of these two functions periodically. To unregister a callback, pass a null pointer as argument.
 * 
 * @param callback : the callback function to call, or a null pointer. The callback function should take two
 *         arguments: the function object of which the value has changed, and the character string describing
 *         the new advertised value.
 * @noreturn
 */
int YFunction::registerValueCallback(YFunctionValueCallback callback)
{
    string val;
    if (callback != NULL) {
        YFunction::_UpdateValueCallbackList(this, true);
    } else {
        YFunction::_UpdateValueCallbackList(this, false);
    }
    _valueCallbackFunction = callback;
    // Immediately invoke value callback with current value
    if (callback != NULL && this->isOnline()) {
        val = _advertisedValue;
        if (!(val == "")) {
            this->_invokeValueCallback(val);
        }
    }
    return 0;
}

int YFunction::_invokeValueCallback(string value)
{
    if (_valueCallbackFunction != NULL) {
        _valueCallbackFunction(this, value);
    } else {
    }
    return 0;
}

int YFunction::_parserHelper(void)
{
    return 0;
}

YFunction *YFunction::nextFunction(void)
{
    string  hwid;
    
    if(YISERR(_nextFunction(hwid)) || hwid=="") {
        return NULL;
    }
    return YFunction::FindFunction(hwid);
}

YFunction* YFunction::FirstFunction(void)
{
    vector<YFUN_DESCR>   v_fundescr;
    YDEV_DESCR             ydevice;
    string              serial, funcId, funcName, funcVal, errmsg;
    
    if(YISERR(YapiWrapper::getFunctionsByClass("Function", 0, v_fundescr, sizeof(YFUN_DESCR), errmsg)) ||
       v_fundescr.size() == 0 ||
       YISERR(YapiWrapper::getFunctionInfo(v_fundescr[0], ydevice, serial, funcId, funcName, funcVal, errmsg))) {
        return NULL;
    }
    return YFunction::FindFunction(serial+"."+funcId);
}

//--- (end of generated code: YFunction implementation)

//--- (generated code: Function functions)
//--- (end of generated code: Function functions)

void YFunction::_throw(YRETCODE errType, string errMsg)
{
    _lastErrorType = errType;
    _lastErrorMsg = errMsg;
    // Method used to throw exceptions or save error type/message
    if(!YAPI::ExceptionsDisabled) {
        throw YAPI_Exception(errType, errMsg);
    }
}

// Method used to resolve our name to our unique function descriptor (may trigger a hub scan)
YRETCODE YFunction::_getDescriptor(YFUN_DESCR& fundescr, string& errmsg)
{
    int res;
    YFUN_DESCR tmp_fundescr;

    tmp_fundescr = YapiWrapper::getFunction(_className, _func, errmsg);
    if(YISERR(tmp_fundescr)) {
        res = YapiWrapper::updateDeviceList(true,errmsg);
        if(YISERR(res)) {
            return (YRETCODE)res;
        }
        tmp_fundescr = YapiWrapper::getFunction(_className, _func, errmsg);
        if(YISERR(tmp_fundescr)) {
            return (YRETCODE)tmp_fundescr;
        }
    }
    _fundescr =  fundescr = tmp_fundescr;
    return YAPI_SUCCESS;
}

// Return a pointer to our device caching object (may trigger a hub scan)
YRETCODE YFunction::_getDevice(YDevice*& dev, string& errmsg)
{
    YFUN_DESCR   fundescr;
    YDEV_DESCR     devdescr;
    YRETCODE    res;

    // Resolve function name
    res = _getDescriptor(fundescr, errmsg);
    if(YISERR(res)) return res;
    
    // Get device descriptor
    devdescr = YapiWrapper::getDeviceByFunction(fundescr, errmsg);
    if(YISERR(devdescr)) return (YRETCODE)devdescr;
    
    // Get device object
    dev = YDevice::getDevice(devdescr);

    return YAPI_SUCCESS;
}

// Return the next known function of current class listed in the yellow pages
YRETCODE YFunction::_nextFunction(string& hwid)
{
    vector<YFUN_DESCR>   v_fundescr;
    YFUN_DESCR           fundescr;
    YDEV_DESCR           devdescr;
    string               serial, funcId, funcName, funcVal, errmsg;
    int                  res;

    res = _getDescriptor(fundescr, errmsg);
    if(YISERR(res)) {
        _throw((YRETCODE)res, errmsg);
        return (YRETCODE)res;
    }
    res = YapiWrapper::getFunctionsByClass(_className, fundescr, v_fundescr, sizeof(YFUN_DESCR), errmsg);
    if(YISERR((YRETCODE)res)) {
        _throw((YRETCODE)res, errmsg);
        return (YRETCODE)res;
    }
    if(v_fundescr.size() == 0) {
        hwid = "";
        return YAPI_SUCCESS;
    }
    res = YapiWrapper::getFunctionInfo(v_fundescr[0], devdescr, serial, funcId, funcName, funcVal, errmsg);
    if(YISERR(res)) {
        _throw((YRETCODE)res, errmsg);
        return (YRETCODE)res;
    }
    hwid = serial+"."+funcId;

    return YAPI_SUCCESS;
}

// Parse a long JSON string
string  YFunction::_parseString(yJsonStateMachine& j)
{
    string  res = j.token;
    
    while(j.next == YJSON_PARSE_STRINGCONT && yJsonParse(&j) == YJSON_PARSE_AVAIL) {
        res += j.token;
    }
    return res;
}

string      YFunction::_json_get_key(const string& json, const string& key)
{
    yJsonStateMachine j;
    
    // Parse JSON data for the device and locate our function in it
    j.src = json.c_str();
    j.end = j.src + strlen(j.src);
    j.st = YJSON_START;
    if(yJsonParse(&j) != YJSON_PARSE_AVAIL || j.st != YJSON_PARSE_STRUCT) {
        this->_throw(YAPI_IO_ERROR,"JSON structure expected");
        return YAPI_INVALID_STRING;
    }
    while(yJsonParse(&j) == YJSON_PARSE_AVAIL && j.st == YJSON_PARSE_MEMBNAME) {
        if (!strcmp(j.token, key.c_str())) {
            if (yJsonParse(&j) != YJSON_PARSE_AVAIL) {
                this->_throw(YAPI_IO_ERROR,"JSON structure expected");
                return YAPI_INVALID_STRING;
            }
            return  _parseString(j);
        }
        yJsonSkip(&j, 1);
    }
    this->_throw(YAPI_IO_ERROR,"invalid JSON structure");
    return YAPI_INVALID_STRING;   
}

string YFunction::_json_get_string(const string& json)
{
    yJsonStateMachine j;
    j.src = json.c_str();
    j.end = j.src + strlen(j.src);
    j.st = YJSON_START;
    if(yJsonParse(&j) != YJSON_PARSE_AVAIL || j.st != YJSON_PARSE_STRING) {
        this->_throw(YAPI_IO_ERROR,"JSON string expected");
        return "";
    }
    return _parseString(j);
}

vector<string> YFunction::_json_get_array(const string& json)
{
    vector<string> res;
    yJsonStateMachine j;
    const char *json_cstr,*last;
    j.src = json_cstr = json.c_str();
    j.end = j.src + strlen(j.src);
    j.st = YJSON_START;
    if(yJsonParse(&j) != YJSON_PARSE_AVAIL || j.st != YJSON_PARSE_ARRAY) {
        this->_throw(YAPI_IO_ERROR,"JSON structure expected");
        return res;
    }
    int depth =j.depth;
    do {
        last=j.src;
        while(yJsonParse(&j) == YJSON_PARSE_AVAIL && j.depth > depth);
        if (j.depth == depth) {
            long location,length;
            while(*last ==',' || *last =='\n')last++;
            location = last -json_cstr;
            length = j.src-last;
            string item = json.substr(location,length);
            res.push_back(item);
        }
    } while (j.st != YJSON_PARSE_ARRAY);
    return res;   
}


YRETCODE  YFunction::_buildSetRequest( const string& changeattr, const string  *changeval, string& request, string& errmsg)
{
    int res;
    YFUN_DESCR fundesc;
    char        funcid[YOCTO_FUNCTION_LEN];
    char        errbuff[YOCTO_ERRMSG_LEN];
    
    
    // Resolve the function name
    res = _getDescriptor(fundesc, errmsg);
    if(YISERR(res)) {
        return (YRETCODE)res;
    }

    if(YISERR(res=yapiGetFunctionInfo(fundesc, NULL, NULL, funcid, NULL, NULL,errbuff))){
        errmsg = errbuff;
        _throw((YRETCODE)res, errmsg);
        return (YRETCODE)res;
    }
    request = "GET /api/";
    request.append(funcid);
    request.append("/");
    //request.append(".json/");
    
    if(changeattr!="") {
        request.append(changeattr);
        if(changeval) {
            const char *p;
            unsigned char        c;
            unsigned char       esc[3];
            request.append("?");
            request.append(changeattr);
            request.append("=");
            esc[0]='%';
            for(p=changeval->c_str(); (c = *p) != 0; p++) {
                if(c <= ' ' || (c > 'z' && c != '~') || c == '"' || c == '%' || c == '&' || 
                   c == '+' || c == '<' || c == '=' || c == '>' || c == '\\' || c == '^' || c == '`') {
                    esc[1]=(c >= 0xa0 ? (c>>4)-10+'A' : (c>>4)+'0');
                    c &= 0xf;
                    esc[2]=(c >= 0xa ? c-10+'A' : c+'0');
                    request.append((char*)esc,3);
                } else {
                    request.append((char*)&c,1);
                }
            }
        }
    }
    // don't append HTTP/1.1 so that we get light headers from hub
    // but append &. suffix to enable connection keepalive on newer hubs
    request.append("&. \r\n\r\n");
    return YAPI_SUCCESS;
}



int YFunction::_parse(yJsonStateMachine& j)
{
    if(yJsonParse(&j) != YJSON_PARSE_AVAIL || j.st != YJSON_PARSE_STRUCT) {
        return -1;
    }
    while(yJsonParse(&j) == YJSON_PARSE_AVAIL && j.st == YJSON_PARSE_MEMBNAME) {
        if (!_parseAttr(j)) {
            // ignore unknown field
            yJsonSkip(&j, 1);
        }
    }
    if(j.st != YJSON_PARSE_STRUCT) 
        return -1;
    this->_parserHelper();
    
    return 0;
}

// Set an attribute in the function, and parse the resulting new function state
YRETCODE YFunction::_setAttr(string attrname, string newvalue)
{
    string      errmsg, request;
    int         res;
    YDevice     *dev;
    
    // Execute http request
    res = _buildSetRequest(attrname, &newvalue, request, errmsg);
    if(YISERR(res)) {
        _throw((YRETCODE)res, errmsg);
        return (YRETCODE)res;
    }
    // Get device Object
    res = _getDevice(dev,  errmsg);
    if(YISERR(res)) {
        _throw((YRETCODE)res, errmsg);
        return (YRETCODE)res;
    }
    
    res = dev->HTTPRequestAsync(request,NULL,NULL,errmsg);
    if(YISERR(res)) {
        // Check if an update of the device list does not solve the issue
        res = YapiWrapper::updateDeviceList(true,errmsg);
        if(YISERR(res)) {
            _throw((YRETCODE)res, errmsg);
            return (YRETCODE)res;
        }
        res = dev->HTTPRequestAsync(request,NULL,NULL,errmsg);
        if(YISERR(res)) {
            _throw((YRETCODE)res, errmsg);
            return (YRETCODE)res;
        }
    }
    if (_cacheExpiration != 0) {
        _cacheExpiration=0;
    }
    return YAPI_SUCCESS;
    
}


// Method used to send http request to the device (not the function)
string      YFunction::_request(const string& request)
{
    YDevice     *dev;
    string      errmsg, buffer;
    int         res;
    
    
    // Resolve our reference to our device, load REST API
    res = _getDevice(dev, errmsg);
    if(YISERR(res)) {
        _throw((YRETCODE)res, errmsg);
        return YAPI_INVALID_STRING;
    }
    res = dev->HTTPRequest(request, buffer, errmsg);
    if(YISERR(res)) {
        // Check if an update of the device list does notb solve the issue
        res = YapiWrapper::updateDeviceList(true,errmsg);
        if(YISERR(res)) {
            this->_throw((YRETCODE)res,errmsg);
            return YAPI_INVALID_STRING;
        }
        res = dev->HTTPRequest(request, buffer, errmsg);
        if(YISERR(res)) {
            this->_throw((YRETCODE)res,errmsg);
            return YAPI_INVALID_STRING;
        }
    }
    if(0 != buffer.find("OK\r\n")){
        if(0 != buffer.find("HTTP/1.1 200 OK\r\n")){
            this->_throw(YAPI_IO_ERROR,"http request failed");
            return YAPI_INVALID_STRING;
        }
    }
    return buffer;
}


// Method used to send http request to the device (not the function)
string      YFunction::_download(const string& url)
{
    string      request,buffer;
	size_t      found;
      
    request = "GET /"+url+" HTTP/1.1\r\n\r\n";
    buffer = this->_request(request);
    found = buffer.find("\r\n\r\n");
    if(string::npos == found){
        this->_throw(YAPI_IO_ERROR,"http request failed");
        return YAPI_INVALID_STRING;
    }
    return buffer.substr(found+4);
}


// Method used to upload a file to the device
YRETCODE    YFunction::_upload(const string& path, const string& content)
{

    string      request,buffer;
    string      boundary;
	size_t      found;
  
    request = "POST /upload.html HTTP/1.1\r\n";
    string body =   "Content-Disposition: form-data; name=\""+path+"\"; filename=\"api\"\r\n"+
                    "Content-Type: application/octet-stream\r\n"+
                    "Content-Transfer-Encoding: binary\r\n\r\n"+content;
    do {
        boundary = YapiWrapper::ysprintf("Zz%06xzZ", rand() & 0xffffff); 
    } while(body.find(boundary) !=string::npos);
    request += "Content-Type: multipart/form-data; boundary="+boundary+"\r\n";
    request += "\r\n--"+boundary+"\r\n"+body+"\r\n--"+boundary+"--\r\n";
    buffer = this->_request(request);
    found = buffer.find("\r\n\r\n");
    if(string::npos == found){
        this->_throw(YAPI_IO_ERROR,"http request failed");
        return YAPI_IO_ERROR;
    }
    return YAPI_SUCCESS;
}


// Method used to cache DataStream objects (new DataLogger)
YDataStream *YFunction::_findDataStream(YDataSet& dataset, const string& def)
{
    string key = dataset.get_functionId()+":"+def;
    if(_dataStreams.find(key) != _dataStreams.end())        
        return _dataStreams[key];
    
    YDataStream *newDataStream = new YDataStream(this, dataset, YAPI::_decodeWords(def));
    _dataStreams[key] = newDataStream;
    return newDataStream;
}



// Return a string that describes the function (class and logical name or hardware id)
string YFunction::describe(void)
{
    YFUN_DESCR  fundescr;
    YDEV_DESCR  devdescr;
    string      errmsg, serial, funcId, funcName, funcValue;
    string      descr =  _func;

    fundescr = YapiWrapper::getFunction(_className, _func, errmsg);
    if(!YISERR(fundescr) && !YISERR(YapiWrapper::getFunctionInfo(fundescr, devdescr, serial, funcId, funcName, funcValue, errmsg))) {
            return _className +"("+_func+")="+serial+"."+funcId;
    }
    return _className +"("+_func+")=unresolved";
}

// Return a string that describes the function (class and logical name or hardware id)
string YFunction::get_friendlyName(void)
{
    YFUN_DESCR   fundescr,moddescr;
    YDEV_DESCR   devdescr;
    YRETCODE     retcode;
    string       errmsg, serial, funcId, funcName, funcValue;
    string       mod_serial, mod_funcId,mod_funcname;
    
    // Resolve the function name
    retcode = _getDescriptor(fundescr, errmsg);
    if(!YISERR(retcode) && !YISERR(YapiWrapper::getFunctionInfo(fundescr, devdescr, serial, funcId, funcName, funcValue, errmsg))) {
        if(funcName!="") {
            funcId = funcName;
        }
        
        moddescr = YapiWrapper::getFunction("Module", serial, errmsg);
        if(!YISERR(moddescr) && !YISERR(YapiWrapper::getFunctionInfo(moddescr, devdescr, mod_serial, mod_funcId, mod_funcname, funcValue, errmsg))) {
            if(mod_funcname!="") {
                return mod_funcname+"."+funcId;
            }
        }
        return serial+"."+funcId;
    }
    _throw((YRETCODE)YAPI::DEVICE_NOT_FOUND, errmsg);
    return Y_FRIENDLYNAME_INVALID;
}




// Returns the unique hardware ID of the function
string YFunction::get_hardwareId(void)
{
    YRETCODE    retcode;
    YFUN_DESCR  fundesc;
    string      errmsg;
    char        snum[YOCTO_SERIAL_LEN];
    char        funcid[YOCTO_FUNCTION_LEN];
    char        errbuff[YOCTO_ERRMSG_LEN];
    
    
    // Resolve the function name
    retcode = _getDescriptor(fundesc, errmsg);
    if(YISERR(retcode)) {
        _throw(retcode, errmsg);
        return HARDWAREID_INVALID;
    }    
    if(YISERR(retcode=yapiGetFunctionInfo(fundesc, NULL, snum, funcid, NULL, NULL,errbuff))){
        errmsg = errbuff;
        _throw(retcode, errmsg);
        return HARDWAREID_INVALID;
    }

    return string(snum)+string(".")+string(funcid);
}

// Returns the unique function ID of the function
string YFunction::get_functionId(void)
{
    YRETCODE    retcode;
    YFUN_DESCR  fundesc;
    string      errmsg;
    char        funcid[YOCTO_FUNCTION_LEN];
    char        errbuff[YOCTO_ERRMSG_LEN];
    
    
    // Resolve the function name
    retcode = _getDescriptor(fundesc, errmsg);
    if(YISERR(retcode)) {
        _throw(retcode, errmsg);
        return HARDWAREID_INVALID;
    }
    if(YISERR(retcode=yapiGetFunctionInfo(fundesc, NULL, NULL, funcid, NULL, NULL,errbuff))){
        errmsg = errbuff;
        _throw(retcode, errmsg);
        return HARDWAREID_INVALID;
    }
    
    return string(funcid);
}


// Return the numerical error type of the last error with this function
YRETCODE YFunction::get_errorType(void)
{
    return _lastErrorType;
}

// Return the human-readable explanation about the last error with this function
string YFunction::get_errorMessage(void)
{
    return _lastErrorMsg;
}

// Return true if the function can be reached, and false otherwise. No exception will be raised.
// If there is a valid value in cache, the device is considered reachable.
bool YFunction::isOnline(void)
{
    YDevice     *dev;
    string      errmsg, apires;

    // A valid value in cache means that the device is online
    if(_cacheExpiration > yapiGetTickCount()) return true;
    
    // Check that the function is available, without throwing exceptions
    if(YISERR(_getDevice(dev, errmsg))) return false;

    // Try to execute a function request to be positively sure that the device is ready
    if(YISERR(dev->requestAPI(apires, errmsg))) {
		return false;
	}

    // Preload the function data, since we have it in device cache
    this->load(YAPI::DefaultCacheValidity);
    
    return true;
}

YRETCODE YFunction::load(int msValidity)
{
    yJsonStateMachine j;
    YDevice     *dev;
    string      errmsg, apires;
    YFUN_DESCR   fundescr;
    int         res;
    char        errbuf[YOCTO_ERRMSG_LEN];
    char        serial[YOCTO_SERIAL_LEN];
    char        funcId[YOCTO_FUNCTION_LEN];

    // Resolve our reference to our device, load REST API
    res = _getDevice(dev, errmsg);
    if(YISERR(res)) {
        _throw((YRETCODE)res, errmsg);
        return (YRETCODE)res;
    }
    res = dev->requestAPI(apires, errmsg);
    if(YISERR(res)) {
        _throw((YRETCODE)res, errmsg);
        return (YRETCODE)res;
    }                
    
    // Get our function Id
    fundescr = YapiWrapper::getFunction(_className, _func, errmsg);
    if(YISERR(fundescr)) {
        _throw((YRETCODE)fundescr, errmsg);
        return (YRETCODE)fundescr;
    }
    res = yapiGetFunctionInfo(fundescr, NULL, serial, funcId, NULL, NULL, errbuf);
    if(YISERR(res)) {
        _throw((YRETCODE)res, errbuf);
        return (YRETCODE)res;
    }            
    _cacheExpiration = yapiGetTickCount() + msValidity;
    _serial = serial;
    _funId = funcId;
    _hwId = _serial + '.' + _funId;

    // Parse JSON data for the device and locate our function in it
    j.src = apires.data();
    j.end = j.src + apires.size();
    j.st = YJSON_START;
    if(yJsonParse(&j) != YJSON_PARSE_AVAIL || j.st != YJSON_PARSE_STRUCT) {
        _throw(YAPI_IO_ERROR, "JSON structure expected");
        return YAPI_IO_ERROR;
    }
    while(yJsonParse(&j) == YJSON_PARSE_AVAIL && j.st == YJSON_PARSE_MEMBNAME) {
        if(!strcmp(j.token, funcId)) {
            _parse(j);
            break;
        }
        yJsonSkip(&j, 1);
    }
    
    return YAPI_SUCCESS;
}

YModule *YFunction::get_module(void)
{
    YFUN_DESCR   fundescr;
    YDEV_DESCR     devdescr;
    string      errmsg, serial, funcId, funcName, funcValue;

    fundescr = YapiWrapper::getFunction(_className, _func, errmsg);
    if(!YISERR(fundescr)) {
        if(!YISERR(YapiWrapper::getFunctionInfo(fundescr, devdescr, serial, funcId, funcName, funcValue, errmsg))) {
            return yFindModule(serial+".module");
        }
    }
    // return a true YModule object even if it is not a module valid for communicating
    return yFindModule(string("module_of_")+_className+"_"+_func);
}



void     *YFunction::get_userData(void)
{
    return _userData;
}


void      YFunction::set_userData(void* data)
{
    _userData = data;
}


YFUN_DESCR YFunction::get_functionDescriptor(void)
{
    return _fundescr;
}

void YFunction::_UpdateValueCallbackList(YFunction* func, bool add)
{
    if (add) {
        func->isOnline();
        vector<YFunction*>::iterator it;
        for ( it=_FunctionCallbacks.begin() ; it < _FunctionCallbacks.end(); it++ ){
            if (*it == func)
                return;
        }
        _FunctionCallbacks.push_back(func);
    }else{
        vector<YFunction*>::iterator it;
        for ( it=_FunctionCallbacks.begin() ; it < _FunctionCallbacks.end(); it++ ){
            if (*it == func) {
                 _FunctionCallbacks.erase(it);
            }
        }
    }
}


void YFunction::_UpdateTimedReportCallbackList(YFunction* func, bool add)
{
  if (add) {
        func->isOnline();
        vector<YFunction*>::iterator it;
        for ( it=_TimedReportCallbackList.begin() ; it < _TimedReportCallbackList.end(); it++ ){
            if (*it == func)
                return;
        }
        _TimedReportCallbackList.push_back(func);
    }else{
        vector<YFunction*>::iterator it;
        for ( it=_TimedReportCallbackList.begin() ; it < _TimedReportCallbackList.end(); it++ ){
            if (*it == func) {
                 _TimedReportCallbackList.erase(it);
            }
        }
    }
}


// This is the internal device cache object
vector<YDevice*> YDevice::_devCache;

YDevice::YDevice(YDEV_DESCR devdesc): _devdescr(devdesc), _cacheStamp(0), _subpath(NULL){ };


YDevice::~YDevice() // destructor
{
    if(_subpath != NULL)
        delete _subpath;
}

void YDevice::ClearCache()
{
    for(unsigned int idx = 0; idx < YDevice::_devCache.size(); idx++) {
        delete _devCache[idx];
    }
    _devCache.clear();
}


YDevice *YDevice::getDevice(YDEV_DESCR devdescr)
{
    // Search in cache
    for(unsigned int idx = 0; idx < YDevice::_devCache.size(); idx++) {
        if(YDevice::_devCache[idx]->_devdescr == devdescr) {
            return YDevice::_devCache[idx];
        }
    }

    // Not found, add new entry
    YDevice *dev = new YDevice(devdescr);
    YDevice::_devCache.push_back(dev);
    
    return dev;
}


YRETCODE    YDevice::HTTPRequestPrepare(const string& request, string& fullrequest, char *errbuff)
{
    YRETCODE    res;
    size_t      pos;
    
    if(_subpath==NULL){
        int neededsize;
        res = yapiGetDevicePath(_devdescr, _rootdevice, NULL, 0, &neededsize, errbuff);
        if(YISERR(res)) return res;
        _subpath = new char[neededsize];
        res = yapiGetDevicePath(_devdescr, _rootdevice, _subpath, neededsize, NULL, errbuff);
        if(YISERR(res)) return res;
    }
    pos = request.find_first_of('/');
    fullrequest = request.substr(0,pos) + (string)_subpath + request.substr(pos+1);

    return YAPI_SUCCESS;
}

YRETCODE    YDevice::HTTPRequestAsync(const string& request, HTTPRequestCallback callback, void *context, string& errmsg)
{
    char        errbuff[YOCTO_ERRMSG_LEN]="";
    YRETCODE    res;
    string      fullrequest;

    _cacheStamp     = YAPI::GetTickCount(); //invalidate cache
    if(YISERR(res=HTTPRequestPrepare(request, fullrequest, errbuff)) ||
       YISERR(res=yapiHTTPRequestAsync(_rootdevice, fullrequest.c_str(), NULL, NULL, errbuff))){
        errmsg = (string)errbuff;
        return res;
    }
    return YAPI_SUCCESS;
}


YRETCODE    YDevice::HTTPRequest(const string& request, string& buffer, string& errmsg)
{
    char        errbuff[YOCTO_ERRMSG_LEN]="";
    YRETCODE    res;
    YIOHDL      iohdl;
    string      fullrequest;
    char        *reply;
    int         replysize = 0;
     
    if(YISERR(res=HTTPRequestPrepare(request, fullrequest, errbuff))) {
        errmsg = (string)errbuff;
        return res;
    }
    if(YISERR(res=yapiHTTPRequestSyncStartEx(&iohdl, _rootdevice, fullrequest.data(), (int)fullrequest.size(), &reply, &replysize, errbuff))) {
        errmsg = (string)errbuff;
        return res;
    }
    buffer = string(reply, replysize);
    if(YISERR(res=yapiHTTPRequestSyncDone(&iohdl, errbuff))) {
        errmsg = (string)errbuff;
        return res;
    }

    return YAPI_SUCCESS;
}


YRETCODE YDevice::requestAPI(string& apires, string& errmsg)
{
    yJsonStateMachine j;
    string          rootdev, request, buffer;
    int             res;
    
    // Check if we have a valid cache value
    if(_cacheStamp > YAPI::GetTickCount()) {
        apires = _cacheJson;
        return YAPI_SUCCESS;
    }
    
    // send request, without HTTP/1.1 suffix to get light headers
    res = this->HTTPRequest("GET /api.json \r\n\r\n", buffer, errmsg);
    if(YISERR(res)) {
        // Check if an update of the device list does not solve the issue
        res = YapiWrapper::updateDeviceList(true,errmsg);
        if(YISERR(res)) {
            return (YRETCODE)res;
        }
        // send request, without HTTP/1.1 suffix to get light headers
        res = this->HTTPRequest("GET /api.json \r\n\r\n", buffer, errmsg);
        if(YISERR(res)) {
            return (YRETCODE)res;
        }
    }
    
    // Parse HTTP header
    j.src = buffer.data();
    j.end = j.src + buffer.size();
    j.st = YJSON_HTTP_START;
    if(yJsonParse(&j) != YJSON_PARSE_AVAIL || j.st != YJSON_HTTP_READ_CODE) {
        errmsg = "Failed to parse HTTP header";
        return YAPI_IO_ERROR;
    }
    if(string(j.token) != "200") {
        errmsg = string("Unexpected HTTP return code: ")+j.token;
        return YAPI_IO_ERROR;
    }
    if(yJsonParse(&j) != YJSON_PARSE_AVAIL || j.st != YJSON_HTTP_READ_MSG) {
        errmsg = "Unexpected HTTP header format";
        return YAPI_IO_ERROR;
    }
    if(yJsonParse(&j) != YJSON_PARSE_AVAIL || j.st != YJSON_PARSE_STRUCT) {
        errmsg = "Unexpected JSON reply format";
        return YAPI_IO_ERROR;
    }
    // we know for sure that the last character parsed was a '{'
    do j.src--; while(j.src[0] != '{');
    apires = string(j.src);    
    
    // store result in cache
    _cacheJson = string(j.src);
    _cacheStamp = yapiGetTickCount() + YAPI::DefaultCacheValidity;
    
    return YAPI_SUCCESS;
}

YRETCODE YDevice::getFunctions(vector<YFUN_DESCR> **functions, string& errmsg)
{        
    if(_functions.size() == 0) {
        int res = YapiWrapper::getFunctionsByDevice(_devdescr, 0, _functions, 64, errmsg);
        if(YISERR(res)) return (YRETCODE)res;
    }
    *functions = &_functions;

    return YAPI_SUCCESS;
}


queue<yapiGlobalEvent>  YAPI::_plug_events;
queue<yapiDataEvent>    YAPI::_data_events;

u64         YAPI::_nextEnum         = 0;
bool        YAPI::_apiInitialized   = false; 

std::map<int,yCalibrationHandler> YAPI::_calibHandlers;
YHubDiscoveryCallback   YAPI::_HubDiscoveryCallback = NULL;


// Default cache validity (in [ms]) before reloading data from device. This saves a lots of trafic.
// Note that a value undger 2 ms makes little sense since a USB bus itself has a 2ms roundtrip period
int YAPI::DefaultCacheValidity = 5; 

// Switch to turn off exceptions and use return codes instead, for source-code compatibility
// with languages without exception support like pure C
bool YAPI::ExceptionsDisabled = false; 

// standard error objects
const string YAPI::INVALID_STRING = YAPI_INVALID_STRING;
const double YAPI::INVALID_DOUBLE = (-DBL_MAX);


yLogFunction            YAPI::LogFunction            = NULL;
yDeviceUpdateCallback   YAPI::DeviceArrivalCallback  = NULL;
yDeviceUpdateCallback   YAPI::DeviceRemovalCallback  = NULL;
yDeviceUpdateCallback   YAPI::DeviceChangeCallback   = NULL;

void YAPI::_yapiLogFunctionFwd(const char *log, u32 loglen)
{
    if(YAPI::LogFunction)
        YAPI::LogFunction(string(log));    
}


void YAPI::_yapiDeviceLogCallbackFwd(YDEV_DESCR devdesc, const char* line)
{
    YModule             *module;
    yDeviceSt           infos;
    string              errmsg;
    YModuleLogCallback  callback;

    if(YapiWrapper::getDeviceInfo(devdesc, infos, errmsg) != YAPI_SUCCESS) return;
    module = YModule::FindModule(string(infos.serial)+".module");
    callback = module->get_logCallback();
    if (callback) {
        callback(module, string(line));
    }
}


void YAPI::_yapiDeviceArrivalCallbackFwd(YDEV_DESCR devdesc)
{
	yapiGlobalEvent    ev;
	yapiDataEvent      dataEv;
    yDeviceSt    infos;
    string       errmsg;
    vector<YFunction*>::iterator it;
    
	dataEv.type = YAPI_FUN_REFRESH;
    for ( it=_FunctionCallbacks.begin() ; it < _FunctionCallbacks.end(); it++ ){
        if ((*it)->functionDescriptor() == Y_FUNCTIONDESCRIPTOR_INVALID){
			dataEv.fun = *it;
			_data_events.push(dataEv);
        }
    }
    if (YAPI::DeviceArrivalCallback == NULL) return;
    ev.type      = YAPI_DEV_ARRIVAL;
    //the function is allready thread safe (use yapiLockDeviceCallaback)
    if(YapiWrapper::getDeviceInfo(devdesc, infos, errmsg) != YAPI_SUCCESS) return;
    ev.module = yFindModule(string(infos.serial)+".module");
    ev.module->setImmutableAttributes(&infos);
    _plug_events.push(ev);
}

void YAPI::_yapiDeviceRemovalCallbackFwd(YDEV_DESCR devdesc)
{
	yapiGlobalEvent    ev;
    yDeviceSt    infos;
    string       errmsg;

    if (YAPI::DeviceRemovalCallback == NULL) return;
    ev.type   = YAPI_DEV_REMOVAL;
    if(YapiWrapper::getDeviceInfo(devdesc, infos, errmsg) != YAPI_SUCCESS) return;
    ev.module = yFindModule(string(infos.serial)+".module");
    //the function is allready thread safe (use yapiLockDeviceCallaback)
    _plug_events.push(ev);
}

void YAPI::_yapiDeviceChangeCallbackFwd(YDEV_DESCR devdesc)
{
	yapiGlobalEvent    ev;
    yDeviceSt    infos;
    string       errmsg;

    if (YAPI::DeviceChangeCallback == NULL) return;
    ev.type      = YAPI_DEV_CHANGE;
    if(YapiWrapper::getDeviceInfo(devdesc, infos, errmsg) != YAPI_SUCCESS) return;
    ev.module = yFindModule(string(infos.serial)+".module");
    ev.module->setImmutableAttributes(&infos);
    //the function is allready thread safe (use yapiLockDeviceCallaback)
    _plug_events.push(ev);
}

void YAPI::_yapiFunctionUpdateCallbackFwd(YAPI_FUNCTION fundesc,const char *value)
{
	yapiDataEvent    ev;

    //the function is allready thread safe (use yapiLockFunctionCallaback)
    if(value==NULL){
        ev.type      = YAPI_FUN_UPDATE;
    }else{
        ev.type      = YAPI_FUN_VALUE;
        memcpy(ev.value,value,YOCTO_PUBVAL_LEN);
    }
    for (unsigned i=0 ; i< _FunctionCallbacks.size();i++) {
        if (_FunctionCallbacks[i]->get_functionDescriptor() == fundesc) {
            ev.fun = _FunctionCallbacks[i];
            _data_events.push(ev);
        }
    }
}

void YAPI::_yapiFunctionTimedReportCallbackFwd(YAPI_FUNCTION fundesc,double timestamp, const u8 *bytes, u32 len)
{
	yapiDataEvent    ev;

    for (unsigned i=0 ; i< _TimedReportCallbackList.size();i++) {
        if (_TimedReportCallbackList[i]->get_functionDescriptor() == fundesc) {
			u32 p = 0;
			ev.type = YAPI_FUN_TIMEDREPORT;
            ev.sensor = (YSensor*)_TimedReportCallbackList[i];
            ev.timestamp =  timestamp;
			ev.len = len;
            while(p < len) {
				ev.report[p++] = *bytes++;
            }
            _data_events.push(ev);
        }
    }
}

void YAPI::_yapiHubDiscoveryCallbackFwd(const char *serial, const char *url)
{
	yapiGlobalEvent    ev;

	if (YAPI::_HubDiscoveryCallback == NULL) return;
	ev.type = YAPI_HUB_DISCOVER;
	strcpy(ev.serial, serial);
	strcpy(ev.url, url);
	_plug_events.push(ev);
}





static double decExp[16] = { 
    1.0e-6, 1.0e-5, 1.0e-4, 1.0e-3, 1.0e-2, 1.0e-1, 1.0, 
    1.0e1, 1.0e2, 1.0e3, 1.0e4, 1.0e5, 1.0e6, 1.0e7, 1.0e8, 1.0e9 };

// Convert Yoctopuce 16-bit decimal floats to standard double-precision floats
//
double YAPI::_decimalToDouble(s16 val)
{
    int     negate = 0;
    double  res;
        
    if(val == 0) return 0.0;
    if(val < 0) {
        negate = 1;
        val = -val;
    }
    res = (double)(val & 2047) * decExp[val >> 11];
    
    return (negate ? -res : res);
}

// Convert standard double-precision floats to Yoctopuce 16-bit decimal floats
//
s16 YAPI::_doubleToDecimal(double val)
{
    int     negate = 0;
    double  comp, mant;
    int     decpow;
    int     res;
    
    if(val == 0.0) {
        return 0;
    }
    if(val < 0) {
        negate = 1;
        val = -val;
    }
    comp = val / 1999.0;
    decpow = 0;
    while(comp > decExp[decpow] && decpow < 15) {
        decpow++;
    }
    mant = val / decExp[decpow];
    if(decpow == 15 && mant > 2047.0) {
        res = (15 << 11) + 2047; // overflow
    } else {
        res = (decpow << 11) + (int)floor(mant+.5);
    }
    return (negate ? -res : res);
}

yCalibrationHandler YAPI::_getCalibrationHandler(int calibType)
{
    if(YAPI::_calibHandlers.find(calibType) == YAPI::_calibHandlers.end()) {
        return NULL;
    }
    return YAPI::_calibHandlers[calibType];
}


 
// Parse an array of u16 encoded in a base64-like string with memory-based compresssion
vector<int> YAPI::_decodeWords(string sdat)
{
    vector<int>     udat;
    
    for(unsigned p = 0; p < sdat.size();) {
        unsigned val;
        unsigned c = sdat[p++];
        if(c == '*') {
            val = 0;
        } else if(c == 'X') {
            val = 0xffff;
        } else if(c == 'Y') {
            val = 0x7fff;
        } else if(c >= 'a') {
            int srcpos = (int)udat.size()-1-(c-'a');
            if(srcpos < 0)
                val = 0;
            else
                val = udat[srcpos];
        } else {
            if(p+2 > sdat.size()) return udat;
            val = (c - '0');
            c = sdat[p++];
            val += (c - '0') << 5;
            c = sdat[p++];
            if(c == 'z') c = '\\';
            val += (c - '0') << 10;
        }
        udat.push_back((int)val);
    }
    return udat;
}


/**
 * Returns the version identifier for the Yoctopuce library in use.
 * The version is a string in the form "Major.Minor.Build",
 * for instance "1.01.5535". For languages using an external
 * DLL (for instance C#, VisualBasic or Delphi), the character string
 * includes as well the DLL version, for instance
 * "1.01.5535 (1.01.5439)".
 * 
 * If you want to verify in your code that the library version is
 * compatible with the version that you have used during development,
 * verify that the major number is strictly equal and that the minor
 * number is greater or equal. The build number is not relevant
 * with respect to the library compatibility.
 * 
 * @return a character string describing the library version.
 */
string YAPI::GetAPIVersion(void)
{
    string version;
    string date;
    YapiWrapper::getAPIVersion(version,date);
    return version;
}


/**
 * Initializes the Yoctopuce programming library explicitly.
 * It is not strictly needed to call yInitAPI(), as the library is
 * automatically  initialized when calling yRegisterHub() for the
 * first time.
 * 
 * When Y_DETECT_NONE is used as detection mode,
 * you must explicitly use yRegisterHub() to point the API to the
 * VirtualHub on which your devices are connected before trying to access them.
 * 
 * @param mode : an integer corresponding to the type of automatic
 *         device detection to use. Possible values are
 *         Y_DETECT_NONE, Y_DETECT_USB, Y_DETECT_NET,
 *         and Y_DETECT_ALL.
 * @param errmsg : a string passed by reference to receive any error message.
 * 
 * @return YAPI_SUCCESS when the call succeeds.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
YRETCODE YAPI::InitAPI(int mode, string& errmsg)
{
    char errbuf[YOCTO_ERRMSG_LEN];
    int  i;
    
    if(YAPI::_apiInitialized) 
        return YAPI_SUCCESS;
    YRETCODE res = yapiInitAPI(mode, errbuf);
    if(YISERR(res)) {
        errmsg = errbuf;
        return res;
    }
    yapiRegisterLogFunction(YAPI::_yapiLogFunctionFwd);
    yapiRegisterDeviceLogCallback(YAPI::_yapiDeviceLogCallbackFwd);
    yapiRegisterDeviceArrivalCallback(YAPI::_yapiDeviceArrivalCallbackFwd);
    yapiRegisterDeviceRemovalCallback(YAPI::_yapiDeviceRemovalCallbackFwd);
    yapiRegisterDeviceChangeCallback(YAPI::_yapiDeviceChangeCallbackFwd);
    yapiRegisterFunctionUpdateCallback(YAPI::_yapiFunctionUpdateCallbackFwd);
	yapiRegisterTimedReportCallback(YAPI::_yapiFunctionTimedReportCallbackFwd);
	yapiRegisterHubDiscoveryCallback(YAPI::_yapiHubDiscoveryCallbackFwd);

    yInitializeCriticalSection(&_updateDeviceList_CS);
    yInitializeCriticalSection(&_handleEvent_CS);
    for(i = 0; i <= 20; i++) {
        YAPI::RegisterCalibrationHandler(i, YAPI::LinearCalibrationHandler);
    }
    YAPI::_apiInitialized = true;

    return YAPI_SUCCESS;
}

/**
 * Frees dynamically allocated memory blocks used by the Yoctopuce library.
 * It is generally not required to call this function, unless you
 * want to free all dynamically allocated memory blocks in order to
 * track a memory leak for instance.
 * You should not call any other library function after calling
 * yFreeAPI(), or your program will crash.
 */
void YAPI::FreeAPI(void)
{
    if(YAPI::_apiInitialized) {
        yapiFreeAPI();
        YAPI::_apiInitialized = false;
        yDeleteCriticalSection(&_updateDeviceList_CS);
        yDeleteCriticalSection(&_handleEvent_CS);
        YDevice::ClearCache();
        YFunction::_ClearCache();
        while (!_plug_events.empty()) {
            _plug_events.pop();
        }
        while (!_data_events.empty()) {
            _data_events.pop();
        }
        _calibHandlers.clear();

    }
}

/**
 * Disables the use of exceptions to report runtime errors.
 * When exceptions are disabled, every function returns a specific
 * error value which depends on its type and which is documented in
 * this reference manual.
 */
void  YAPI::DisableExceptions(void)
{ YAPI::ExceptionsDisabled = true; }

/**
 * Re-enables the use of exceptions for runtime error handling.
 * Be aware than when exceptions are enabled, every function that fails
 * triggers an exception. If the exception is not caught by the user code,
 * it  either fires the debugger or aborts (i.e. crash) the program.
 * On failure, throws an exception or returns a negative error code.
 */
void YAPI::EnableExceptions(void)
{ YAPI::ExceptionsDisabled = false; }

/**
 * Registers a log callback function. This callback will be called each time
 * the API have something to say. Quite useful to debug the API.
 * 
 * @param logfun : a procedure taking a string parameter, or null
 *         to unregister a previously registered  callback.
 */
void YAPI::RegisterLogFunction(yLogFunction logfun)
{
    YAPI::LogFunction = logfun;
}

/**
 * Register a callback function, to be called each time
 * a device is plugged. This callback will be invoked while yUpdateDeviceList
 * is running. You will have to call this function on a regular basis.
 * 
 * @param arrivalCallback : a procedure taking a YModule parameter, or null
 *         to unregister a previously registered  callback.
 */
void YAPI::RegisterDeviceArrivalCallback(yDeviceUpdateCallback arrivalCallback)
{
    YAPI::DeviceArrivalCallback = arrivalCallback;
    if(arrivalCallback) {
        YModule *mod =YModule::FirstModule();
        while(mod){
            if(mod->isOnline()){
                yapiLockDeviceCallBack(NULL);
                _yapiDeviceArrivalCallbackFwd(mod->functionDescriptor());
                yapiUnlockDeviceCallBack(NULL);
            }
            mod = mod->nextModule();
        }
    }
}

/**
 * Register a callback function, to be called each time
 * a device is unplugged. This callback will be invoked while yUpdateDeviceList
 * is running. You will have to call this function on a regular basis.
 * 
 * @param removalCallback : a procedure taking a YModule parameter, or null
 *         to unregister a previously registered  callback.
 */
void YAPI::RegisterDeviceRemovalCallback(yDeviceUpdateCallback removalCallback)
{
    YAPI::DeviceRemovalCallback = removalCallback;
}

void YAPI::RegisterDeviceChangeCallback(yDeviceUpdateCallback changeCallback)
{
    YAPI::DeviceChangeCallback = changeCallback;
}

/**
 * Register a callback function, to be called each time an Network Hub send
 * an SSDP message. The callback has two string parameter, the first one
 * contain the serial number of the hub and the second contain the URL of the
 * network hub (this URL can be passed to RegisterHub). This callback will be invoked
 * while yUpdateDeviceList is running. You will have to call this function on a regular basis.
 * 
 * @param hubDiscoveryCallback : a procedure taking two string parameter, or null
 *         to unregister a previously registered  callback.
 */
void YAPI::RegisterHubDiscoveryCallback(YHubDiscoveryCallback hubDiscoveryCallback)
{ 
    YAPI::_HubDiscoveryCallback =  hubDiscoveryCallback;
	string error;
	YAPI::TriggerHubDiscovery(error);
}

/**
 * Force a hub discovery, if a callback as been registered with yRegisterDeviceRemovalCallback it
 * will be called for each net work hub that will respond to the discovery.
 * 
 * @param errmsg : a string passed by reference to receive any error message.
 * 
 * @return YAPI_SUCCESS when the call succeeds.
 *         On failure, throws an exception or returns a negative error code.
 */
YRETCODE YAPI::TriggerHubDiscovery(string& errmsg)
{
	YRETCODE res;
	char errbuf[YOCTO_ERRMSG_LEN];
	if (!YAPI::_apiInitialized) {
		res = YAPI::InitAPI(0, errmsg);
		if (YISERR(res)) return res;
	}
	res = yapiTriggerHubDiscovery(errbuf);
    if(YISERR(res)) {
        errmsg = errbuf;
        return res;
    }
    return YAPI_SUCCESS;
}



// Register a new value calibration handler for a given calibration type
//
void YAPI::RegisterCalibrationHandler(int calibrationType, yCalibrationHandler calibrationHandler)
{
    YAPI::_calibHandlers[calibrationType] = calibrationHandler;
}

// Standard value calibration handler (n-point linear error correction)
//
double YAPI::LinearCalibrationHandler(double rawValue, int calibType, intArr params, floatArr rawValues, floatArr refValues)
{
    // calibration types n=1..10 and 11.20 are meant for linear calibration using n points
    int    npt = calibType % 10;
    double x   = rawValues[0];
    double adj = refValues[0] - x;
    int    i   = 0;
    
    if(npt > (int)rawValues.size()) npt = (int)rawValues.size();
    if(npt > (int)refValues.size()) npt = (int)refValues.size();
    while(rawValue > rawValues[i] && ++i < npt) {
        double x2   = x;
        double adj2 = adj;
        
        x   = rawValues[i];
        adj = refValues[i] - x;
        
        if(rawValue < x && x > x2) {
            adj = adj2 + (adj - adj2) * (rawValue - x2) / (x - x2);
        }
    }
    return rawValue + adj;
}


/**
 * Setup the Yoctopuce library to use modules connected on a given machine. The
 * parameter will determine how the API will work. Use the following values:
 * 
 * <b>usb</b>: When the usb keyword is used, the API will work with
 * devices connected directly to the USB bus. Some programming languages such a Javascript,
 * PHP, and Java don't provide direct access to USB hardware, so usb will
 * not work with these. In this case, use a VirtualHub or a networked YoctoHub (see below).
 * 
 * <b><i>x.x.x.x</i></b> or <b><i>hostname</i></b>: The API will use the devices connected to the
 * host with the given IP address or hostname. That host can be a regular computer
 * running a VirtualHub, or a networked YoctoHub such as YoctoHub-Ethernet or
 * YoctoHub-Wireless. If you want to use the VirtualHub running on you local
 * computer, use the IP address 127.0.0.1.
 * 
 * <b>callback</b>: that keyword make the API run in "<i>HTTP Callback</i>" mode.
 * This a special mode allowing to take control of Yoctopuce devices
 * through a NAT filter when using a VirtualHub or a networked YoctoHub. You only
 * need to configure your hub to call your server script on a regular basis.
 * This mode is currently available for PHP and Node.JS only.
 * 
 * Be aware that only one application can use direct USB access at a
 * given time on a machine. Multiple access would cause conflicts
 * while trying to access the USB modules. In particular, this means
 * that you must stop the VirtualHub software before starting
 * an application that uses direct USB access. The workaround
 * for this limitation is to setup the library to use the VirtualHub
 * rather than direct USB access.
 * 
 * If access control has been activated on the hub, virtual or not, you want to
 * reach, the URL parameter should look like:
 * 
 * http://username:password@adresse:port
 * 
 * You can call <i>RegisterHub</i> several times to connect to several machines.
 * 
 * @param url : a string containing either "usb","callback" or the
 *         root URL of the hub to monitor
 * @param errmsg : a string passed by reference to receive any error message.
 * 
 * @return YAPI_SUCCESS when the call succeeds.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
YRETCODE YAPI::RegisterHub(const string& url, string& errmsg)
{
    char        errbuf[YOCTO_ERRMSG_LEN];
    YRETCODE    res;
    if(!YAPI::_apiInitialized) {
        res = YAPI::InitAPI(0, errmsg);
        if(YISERR(res)) return res;
    }
    res = yapiRegisterHub(url.c_str(), errbuf);
    if(YISERR(res)) {
        errmsg = errbuf;
    }
    return res;
}

/**
 * Fault-tolerant alternative to RegisterHub(). This function has the same
 * purpose and same arguments as RegisterHub(), but does not trigger
 * an error when the selected hub is not available at the time of the function call.
 * This makes it possible to register a network hub independently of the current
 * connectivity, and to try to contact it only when a device is actively needed.
 * 
 * @param url : a string containing either "usb","callback" or the
 *         root URL of the hub to monitor
 * @param errmsg : a string passed by reference to receive any error message.
 * 
 * @return YAPI_SUCCESS when the call succeeds.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
YRETCODE YAPI::PreregisterHub(const string& url, string& errmsg)
{
    char        errbuf[YOCTO_ERRMSG_LEN];
    YRETCODE    res;
    if(!YAPI::_apiInitialized) {
        res = YAPI::InitAPI(0, errmsg);
        if(YISERR(res)) return res;
    }
    res = yapiPreregisterHub(url.c_str(), errbuf);
    if(YISERR(res)) {
        errmsg = errbuf;
    }
    return res;
}
/**
 * Setup the Yoctopuce library to no more use modules connected on a previously
 * registered machine with RegisterHub.
 * 
 * @param url : a string containing either "usb" or the
 *         root URL of the hub to monitor
 */
void YAPI::UnregisterHub(const string& url)
{
    if(!YAPI::_apiInitialized){
        return;
    }
    yapiUnregisterHub(url.c_str());
}


/**
 * Triggers a (re)detection of connected Yoctopuce modules.
 * The library searches the machines or USB ports previously registered using
 * yRegisterHub(), and invokes any user-defined callback function
 * in case a change in the list of connected devices is detected.
 * 
 * This function can be called as frequently as desired to refresh the device list
 * and to make the application aware of hot-plug events.
 * 
 * @param errmsg : a string passed by reference to receive any error message.
 * 
 * @return YAPI_SUCCESS when the call succeeds.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
YRETCODE YAPI::UpdateDeviceList(string& errmsg)
{
    if(!YAPI::_apiInitialized) {
        YRETCODE res = YAPI::InitAPI(0, errmsg);
        if(YISERR(res)) return res;
    }
    // prevent reentrance into this function
    yEnterCriticalSection(&_updateDeviceList_CS);
    // call the updateDeviceList of the yapi layer
    // yapi know when it is needed to do a full update
    YRETCODE res = YapiWrapper::updateDeviceList(false,errmsg);
    if(YISERR(res)) {
        yLeaveCriticalSection(&_updateDeviceList_CS);
        return res;
    }
    // handle other notification
    res = YapiWrapper::handleEvents(errmsg);
    if(YISERR(res)) {
        yLeaveCriticalSection(&_updateDeviceList_CS);
        return res;
    }
    // unpop plug/unplug event and call user callback
    while(!_plug_events.empty()){
        yapiGlobalEvent ev;
        yapiLockDeviceCallBack(NULL);
        if(_plug_events.empty()){
            yapiUnlockDeviceCallBack(NULL);
            break;
        }
        ev = _plug_events.front();
        _plug_events.pop();
        yapiUnlockDeviceCallBack(NULL);
        switch(ev.type){
            case YAPI_DEV_ARRIVAL:
                if(!YAPI::DeviceArrivalCallback) break;
                YAPI::DeviceArrivalCallback(ev.module);    
                break;
            case YAPI_DEV_REMOVAL:
                if(!YAPI::DeviceRemovalCallback) break;
                YAPI::DeviceRemovalCallback(ev.module);    
                break;
            case YAPI_DEV_CHANGE:
                if(!YAPI::DeviceChangeCallback) break;
                YAPI::DeviceChangeCallback(ev.module);    
                break;
			case YAPI_HUB_DISCOVER:
				if (!YAPI::_HubDiscoveryCallback) break;
				YAPI::_HubDiscoveryCallback(string(ev.serial),string(ev.url));
				break;
			default:
                break;
        }
    }
    yLeaveCriticalSection(&_updateDeviceList_CS);
    return YAPI_SUCCESS;
}

/**
 * Maintains the device-to-library communication channel.
 * If your program includes significant loops, you may want to include
 * a call to this function to make sure that the library takes care of
 * the information pushed by the modules on the communication channels.
 * This is not strictly necessary, but it may improve the reactivity
 * of the library for the following commands.
 * 
 * This function may signal an error in case there is a communication problem
 * while contacting a module.
 * 
 * @param errmsg : a string passed by reference to receive any error message.
 * 
 * @return YAPI_SUCCESS when the call succeeds.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
YRETCODE YAPI::HandleEvents(string& errmsg)
{
    YRETCODE    res;
    
    // prevent reentrance into this function
    yEnterCriticalSection(&_handleEvent_CS);
     // handle other notification
    res = YapiWrapper::handleEvents(errmsg);
    if(YISERR(res)) {
        yLeaveCriticalSection(&_handleEvent_CS);
        return res;
    }
    // pop data event and call user callback
    while (!_data_events.empty()) {
        yapiDataEvent   ev;
		YSensor			*sensor;
		vector<int> report;

		yapiLockFunctionCallBack(NULL);
        if (_data_events.empty()) {
            yapiUnlockFunctionCallBack(NULL);
            break;
        }
        ev = _data_events.front();
        _data_events.pop();
        yapiUnlockFunctionCallBack(NULL);
        switch (ev.type) {
            case YAPI_FUN_VALUE:
                ev.fun->_invokeValueCallback((string)ev.value);
                break;
            case YAPI_FUN_TIMEDREPORT:
                sensor = ev.sensor;
				report.assign(ev.report, ev.report + ev.len);
				sensor->_invokeTimedReportCallback(sensor->_decodeTimedReport(ev.timestamp, report));
                break;
            case YAPI_FUN_REFRESH:
                ev.fun->isOnline();
                break;
            default:
                break;
        }
    }
    yLeaveCriticalSection(&_handleEvent_CS); 
    return YAPI_SUCCESS;
}

/**
 * Pauses the execution flow for a specified duration.
 * This function implements a passive waiting loop, meaning that it does not
 * consume CPU cycles significantly. The processor is left available for
 * other threads and processes. During the pause, the library nevertheless
 * reads from time to time information from the Yoctopuce modules by
 * calling yHandleEvents(), in order to stay up-to-date.
 * 
 * This function may signal an error in case there is a communication problem
 * while contacting a module.
 * 
 * @param ms_duration : an integer corresponding to the duration of the pause,
 *         in milliseconds.
 * @param errmsg : a string passed by reference to receive any error message.
 * 
 * @return YAPI_SUCCESS when the call succeeds.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
YRETCODE YAPI::Sleep(unsigned ms_duration, string& errmsg)
{
    char         errbuf[YOCTO_ERRMSG_LEN];
    YRETCODE    res;

    u64         waituntil=YAPI::GetTickCount()+ms_duration;
    do{
       res = YAPI::HandleEvents(errmsg);
        if(YISERR(res)) {
            errmsg = errbuf;
            return res;
        }
        if(waituntil>YAPI::GetTickCount()){
            res = yapiSleep(3, errbuf);
            if(YISERR(res)) {
                errmsg = errbuf;
                return res;
            }
        }
    }while(waituntil>YAPI::GetTickCount());
     
    return YAPI_SUCCESS;
}

/**
 * Returns the current value of a monotone millisecond-based time counter.
 * This counter can be used to compute delays in relation with
 * Yoctopuce devices, which also uses the millisecond as timebase.
 * 
 * @return a long integer corresponding to the millisecond counter.
 */
u64 YAPI::GetTickCount(void)
{
    return yapiGetTickCount();
}

/**
 * Checks if a given string is valid as logical name for a module or a function.
 * A valid logical yocto3d/name.has a maximum of 19 characters, all among
 * A..Z, a..z, 0..9, _, and -.
 * If you try to configure a logical name with an incorrect string,
 * the invalid characters are ignored.
 * 
 * @param name : a string containing the name to check.
 * 
 * @return true if the name is valid, false otherwise.
 */
bool YAPI::CheckLogicalName(const string& name)
{
    return (yapiCheckLogicalName(name.c_str())!=0);
}

u16 YapiWrapper::getAPIVersion(string& version,string& date)
{
    const char  *_ver, *_date;    
    u16 res = yapiGetAPIVersion(&_ver, &_date);
    version     = _ver;
    date        = _date;    
    return res;
}

YDEV_DESCR YapiWrapper::getDevice(const string& device_str, string& errmsg)
{
    char        errbuf[YOCTO_ERRMSG_LEN];
    YDEV_DESCR     res;
    
    res = yapiGetDevice(device_str.data(), errbuf);
    if(YISERR(res)) {
        errmsg = errbuf;
    }
    return (YDEV_DESCR)res;
}

int YapiWrapper::getAllDevices(vector<YDEV_DESCR>& buffer, string& errmsg)
{
    char    errbuf[YOCTO_ERRMSG_LEN];
    int     n_elems = 32;
    int     initsize = n_elems * sizeof(YDEV_DESCR);
    int     neededsize, res;
    YDEV_DESCR *ptr = new YDEV_DESCR[n_elems];
    
    res = yapiGetAllDevices(ptr, initsize, &neededsize, errbuf);
    if(YISERR(res)) {
        delete [] ptr;
        errmsg = errbuf;
        return (YRETCODE)res;
    }
    if(neededsize > initsize) {
        delete [] ptr;
        n_elems = neededsize / sizeof(YDEV_DESCR);
        initsize = n_elems * sizeof(YDEV_DESCR);
        ptr = new YDEV_DESCR[n_elems];
        res = yapiGetAllDevices(ptr, initsize, NULL, errbuf);
        if(YISERR(res)) {
            delete [] ptr;
            errmsg = errbuf;
            return (YRETCODE)res;
        }
    }
    buffer = vector<YDEV_DESCR>(ptr, ptr+res);
    delete [] ptr;
    
    return res;
}

YRETCODE YapiWrapper::getDeviceInfo(YDEV_DESCR devdesc, yDeviceSt& infos, string& errmsg)
{
    char        errbuf[YOCTO_ERRMSG_LEN];
    YRETCODE    res;
    
    res = yapiGetDeviceInfo(devdesc, &infos, errbuf);
    if(YISERR(res)) {
        errmsg = errbuf;
    }
    return res;
}

YFUN_DESCR YapiWrapper::getFunction(const string& class_str, const string& function_str, string& errmsg)
{
    char errbuf[YOCTO_ERRMSG_LEN];
    
    YFUN_DESCR res = yapiGetFunction(class_str.data(), function_str.data(), errbuf);
    if(YISERR(res)) {
        errmsg = errbuf;
    }
    return res;
}

int YapiWrapper::getFunctionsByClass(const string& class_str, YFUN_DESCR prevfundesc, vector<YFUN_DESCR>& buffer, int maxsize, string& errmsg)
{
    char    errbuf[YOCTO_ERRMSG_LEN];
    int     n_elems = 32;
    int     initsize = n_elems * sizeof(YDEV_DESCR);
    int     neededsize;
    YFUN_DESCR *ptr = new YFUN_DESCR[n_elems];
    
    int res = yapiGetFunctionsByClass(class_str.data(), prevfundesc, ptr, initsize, &neededsize, errbuf);
    if(YISERR(res)) {
        delete [] ptr;
        errmsg = errbuf;
        return res;
    }
    if(neededsize > initsize) {
        delete [] ptr;
        n_elems = neededsize / sizeof(YFUN_DESCR);
        initsize = n_elems * sizeof(YFUN_DESCR);
        ptr = new YFUN_DESCR[n_elems];
        res = yapiGetFunctionsByClass(class_str.data(), prevfundesc, ptr, initsize, NULL, errbuf);
        if(YISERR(res)) {
            delete [] ptr;
            errmsg = errbuf;
            return res;
        }
    }
    buffer = vector<YFUN_DESCR>(ptr, ptr+res);
    delete [] ptr;
    
    return res;
}

int YapiWrapper::getFunctionsByDevice(YDEV_DESCR devdesc, YFUN_DESCR prevfundesc, vector<YFUN_DESCR>& buffer, int maxsize, string& errmsg)
{
    char    errbuf[YOCTO_ERRMSG_LEN];
    int     n_elems = 32;
    int     initsize = n_elems * sizeof(YDEV_DESCR);
    int     neededsize;
    YFUN_DESCR *ptr = new YFUN_DESCR[n_elems];
    
    int res = yapiGetFunctionsByDevice(devdesc, prevfundesc, ptr, initsize, &neededsize, errbuf);
    if(YISERR(res)) {
        delete [] ptr;
        errmsg = errbuf;
        return res;
    }
    if(neededsize > initsize) {
        delete [] ptr;
        n_elems = neededsize / sizeof(YFUN_DESCR);
        initsize = n_elems * sizeof(YFUN_DESCR);
        ptr = new YFUN_DESCR[n_elems];
        res = yapiGetFunctionsByDevice(devdesc, prevfundesc, ptr, initsize, NULL, errbuf);
        if(YISERR(res)) {
            delete [] ptr;
            errmsg = errbuf;
            return res;
        }
    }
    buffer = vector<YFUN_DESCR>(ptr, ptr+res);
    delete [] ptr;
    
    return res;
}

YDEV_DESCR YapiWrapper::getDeviceByFunction(YFUN_DESCR fundesc, string& errmsg)
{
    char    errbuf[YOCTO_ERRMSG_LEN];
    YDEV_DESCR dev;
    
    int res = yapiGetFunctionInfo(fundesc, &dev, NULL, NULL, NULL, NULL, errbuf);
    if(YISERR(res)) {
        errmsg = errbuf;
        return res;
    }
    
    return dev;
}

YRETCODE YapiWrapper::getFunctionInfo(YFUN_DESCR fundesc, YDEV_DESCR& devdescr, string& serial, string& funcId, string& funcName, string& funcVal, string& errmsg)
{
    char    errbuf[YOCTO_ERRMSG_LEN];
    char    snum[YOCTO_SERIAL_LEN];
    char    fnid[YOCTO_FUNCTION_LEN];
    char    fnam[YOCTO_LOGICAL_LEN];
    char    fval[YOCTO_PUBVAL_LEN];
    
    YRETCODE res = yapiGetFunctionInfo(fundesc, &devdescr, snum, fnid, fnam, fval, errbuf);
    if(YISERR(res)) {
        errmsg = errbuf;
    } else {
        serial = snum;
        funcId = fnid;
        funcName = fnam;
        funcVal = fval;
    }
    
    return res;
}

YRETCODE YapiWrapper::updateDeviceList(bool forceupdate,string& errmsg)
{
    char        errbuf[YOCTO_ERRMSG_LEN];
    YRETCODE    res = yapiUpdateDeviceList(forceupdate?1:0,errbuf);
    if(YISERR(res)) {
        errmsg = errbuf;
        return res;
    }
    return YAPI_SUCCESS;
}

YRETCODE YapiWrapper::handleEvents(string& errmsg)
{
    char        errbuf[YOCTO_ERRMSG_LEN];
    YRETCODE    res = yapiHandleEvents(errbuf);
    if(YISERR(res)) {
        errmsg = errbuf;
        return res;
    }
    return YAPI_SUCCESS;
}


string  YapiWrapper::ysprintf(const char *fmt, ...)
{
    va_list     args;
    char        buffer[2048];
    va_start( args, fmt );
    vsprintf(buffer,fmt,args);
    va_end(args);
    return (string)buffer;
}






//--- (generated code: YModule constructor)
YModule::YModule(const string& func): YFunction(func)
//--- (end of generated code: YModule constructor)
//--- (generated code: Module initialization)
    ,_productName(PRODUCTNAME_INVALID)
    ,_serialNumber(SERIALNUMBER_INVALID)
    ,_productId(PRODUCTID_INVALID)
    ,_productRelease(PRODUCTRELEASE_INVALID)
    ,_firmwareRelease(FIRMWARERELEASE_INVALID)
    ,_persistentSettings(PERSISTENTSETTINGS_INVALID)
    ,_luminosity(LUMINOSITY_INVALID)
    ,_beacon(BEACON_INVALID)
    ,_upTime(UPTIME_INVALID)
    ,_usbCurrent(USBCURRENT_INVALID)
    ,_rebootCountdown(REBOOTCOUNTDOWN_INVALID)
    ,_usbBandwidth(USBBANDWIDTH_INVALID)
    ,_valueCallbackModule(NULL)
    ,_logCallback(NULL)
//--- (end of generated code: Module initialization)
{
    _className = "Module";
} 

YModule::~YModule()
{
    //--- (generated code: YModule cleanup)
//--- (end of generated code: YModule cleanup)
}




//--- (generated code: YModule implementation)
// static attributes
const string YModule::PRODUCTNAME_INVALID = YAPI_INVALID_STRING;
const string YModule::SERIALNUMBER_INVALID = YAPI_INVALID_STRING;
const string YModule::FIRMWARERELEASE_INVALID = YAPI_INVALID_STRING;

int YModule::_parseAttr(yJsonStateMachine& j)
{
    if(!strcmp(j.token, "productName")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _productName =  _parseString(j);
        return 1;
    }
    if(!strcmp(j.token, "serialNumber")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _serialNumber =  _parseString(j);
        return 1;
    }
    if(!strcmp(j.token, "productId")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _productId =  atoi(j.token);
        return 1;
    }
    if(!strcmp(j.token, "productRelease")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _productRelease =  atoi(j.token);
        return 1;
    }
    if(!strcmp(j.token, "firmwareRelease")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _firmwareRelease =  _parseString(j);
        return 1;
    }
    if(!strcmp(j.token, "persistentSettings")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _persistentSettings =  (Y_PERSISTENTSETTINGS_enum)atoi(j.token);
        return 1;
    }
    if(!strcmp(j.token, "luminosity")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _luminosity =  atoi(j.token);
        return 1;
    }
    if(!strcmp(j.token, "beacon")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _beacon =  (Y_BEACON_enum)atoi(j.token);
        return 1;
    }
    if(!strcmp(j.token, "upTime")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _upTime =  atol(j.token);
        return 1;
    }
    if(!strcmp(j.token, "usbCurrent")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _usbCurrent =  atoi(j.token);
        return 1;
    }
    if(!strcmp(j.token, "rebootCountdown")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _rebootCountdown =  atoi(j.token);
        return 1;
    }
    if(!strcmp(j.token, "usbBandwidth")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _usbBandwidth =  (Y_USBBANDWIDTH_enum)atoi(j.token);
        return 1;
    }
    failed:
    return YFunction::_parseAttr(j);
}


/**
 * Returns the commercial name of the module, as set by the factory.
 * 
 * @return a string corresponding to the commercial name of the module, as set by the factory
 * 
 * On failure, throws an exception or returns Y_PRODUCTNAME_INVALID.
 */
string YModule::get_productName(void)
{
    if (_cacheExpiration == 0) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YModule::PRODUCTNAME_INVALID;
        }
    }
    return _productName;
}

/**
 * Returns the serial number of the module, as set by the factory.
 * 
 * @return a string corresponding to the serial number of the module, as set by the factory
 * 
 * On failure, throws an exception or returns Y_SERIALNUMBER_INVALID.
 */
string YModule::get_serialNumber(void)
{
    if (_cacheExpiration == 0) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YModule::SERIALNUMBER_INVALID;
        }
    }
    return _serialNumber;
}

/**
 * Returns the USB device identifier of the module.
 * 
 * @return an integer corresponding to the USB device identifier of the module
 * 
 * On failure, throws an exception or returns Y_PRODUCTID_INVALID.
 */
int YModule::get_productId(void)
{
    if (_cacheExpiration == 0) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YModule::PRODUCTID_INVALID;
        }
    }
    return _productId;
}

/**
 * Returns the hardware release version of the module.
 * 
 * @return an integer corresponding to the hardware release version of the module
 * 
 * On failure, throws an exception or returns Y_PRODUCTRELEASE_INVALID.
 */
int YModule::get_productRelease(void)
{
    if (_cacheExpiration == 0) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YModule::PRODUCTRELEASE_INVALID;
        }
    }
    return _productRelease;
}

/**
 * Returns the version of the firmware embedded in the module.
 * 
 * @return a string corresponding to the version of the firmware embedded in the module
 * 
 * On failure, throws an exception or returns Y_FIRMWARERELEASE_INVALID.
 */
string YModule::get_firmwareRelease(void)
{
    if (_cacheExpiration <= YAPI::GetTickCount()) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YModule::FIRMWARERELEASE_INVALID;
        }
    }
    return _firmwareRelease;
}

/**
 * Returns the current state of persistent module settings.
 * 
 * @return a value among Y_PERSISTENTSETTINGS_LOADED, Y_PERSISTENTSETTINGS_SAVED and
 * Y_PERSISTENTSETTINGS_MODIFIED corresponding to the current state of persistent module settings
 * 
 * On failure, throws an exception or returns Y_PERSISTENTSETTINGS_INVALID.
 */
Y_PERSISTENTSETTINGS_enum YModule::get_persistentSettings(void)
{
    if (_cacheExpiration <= YAPI::GetTickCount()) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YModule::PERSISTENTSETTINGS_INVALID;
        }
    }
    return _persistentSettings;
}

int YModule::set_persistentSettings(Y_PERSISTENTSETTINGS_enum newval)
{
    string rest_val;
    char buf[32]; sprintf(buf, "%d", newval); rest_val = string(buf);
    return _setAttr("persistentSettings", rest_val);
}

/**
 * Returns the luminosity of the  module informative leds (from 0 to 100).
 * 
 * @return an integer corresponding to the luminosity of the  module informative leds (from 0 to 100)
 * 
 * On failure, throws an exception or returns Y_LUMINOSITY_INVALID.
 */
int YModule::get_luminosity(void)
{
    if (_cacheExpiration <= YAPI::GetTickCount()) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YModule::LUMINOSITY_INVALID;
        }
    }
    return _luminosity;
}

/**
 * Changes the luminosity of the module informative leds. The parameter is a
 * value between 0 and 100.
 * Remember to call the saveToFlash() method of the module if the
 * modification must be kept.
 * 
 * @param newval : an integer corresponding to the luminosity of the module informative leds
 * 
 * @return YAPI_SUCCESS if the call succeeds.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
int YModule::set_luminosity(int newval)
{
    string rest_val;
    char buf[32]; sprintf(buf, "%d", newval); rest_val = string(buf);
    return _setAttr("luminosity", rest_val);
}

/**
 * Returns the state of the localization beacon.
 * 
 * @return either Y_BEACON_OFF or Y_BEACON_ON, according to the state of the localization beacon
 * 
 * On failure, throws an exception or returns Y_BEACON_INVALID.
 */
Y_BEACON_enum YModule::get_beacon(void)
{
    if (_cacheExpiration <= YAPI::GetTickCount()) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YModule::BEACON_INVALID;
        }
    }
    return _beacon;
}

/**
 * Turns on or off the module localization beacon.
 * 
 * @param newval : either Y_BEACON_OFF or Y_BEACON_ON
 * 
 * @return YAPI_SUCCESS if the call succeeds.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
int YModule::set_beacon(Y_BEACON_enum newval)
{
    string rest_val;
    rest_val = (newval>0 ? "1" : "0");
    return _setAttr("beacon", rest_val);
}

/**
 * Returns the number of milliseconds spent since the module was powered on.
 * 
 * @return an integer corresponding to the number of milliseconds spent since the module was powered on
 * 
 * On failure, throws an exception or returns Y_UPTIME_INVALID.
 */
s64 YModule::get_upTime(void)
{
    if (_cacheExpiration <= YAPI::GetTickCount()) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YModule::UPTIME_INVALID;
        }
    }
    return _upTime;
}

/**
 * Returns the current consumed by the module on the USB bus, in milli-amps.
 * 
 * @return an integer corresponding to the current consumed by the module on the USB bus, in milli-amps
 * 
 * On failure, throws an exception or returns Y_USBCURRENT_INVALID.
 */
int YModule::get_usbCurrent(void)
{
    if (_cacheExpiration <= YAPI::GetTickCount()) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YModule::USBCURRENT_INVALID;
        }
    }
    return _usbCurrent;
}

/**
 * Returns the remaining number of seconds before the module restarts, or zero when no
 * reboot has been scheduled.
 * 
 * @return an integer corresponding to the remaining number of seconds before the module restarts, or zero when no
 *         reboot has been scheduled
 * 
 * On failure, throws an exception or returns Y_REBOOTCOUNTDOWN_INVALID.
 */
int YModule::get_rebootCountdown(void)
{
    if (_cacheExpiration <= YAPI::GetTickCount()) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YModule::REBOOTCOUNTDOWN_INVALID;
        }
    }
    return _rebootCountdown;
}

int YModule::set_rebootCountdown(int newval)
{
    string rest_val;
    char buf[32]; sprintf(buf, "%d", newval); rest_val = string(buf);
    return _setAttr("rebootCountdown", rest_val);
}

/**
 * Returns the number of USB interfaces used by the module.
 * 
 * @return either Y_USBBANDWIDTH_SIMPLE or Y_USBBANDWIDTH_DOUBLE, according to the number of USB
 * interfaces used by the module
 * 
 * On failure, throws an exception or returns Y_USBBANDWIDTH_INVALID.
 */
Y_USBBANDWIDTH_enum YModule::get_usbBandwidth(void)
{
    if (_cacheExpiration <= YAPI::GetTickCount()) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YModule::USBBANDWIDTH_INVALID;
        }
    }
    return _usbBandwidth;
}

/**
 * Allows you to find a module from its serial number or from its logical name.
 * 
 * This function does not require that the module is online at the time
 * it is invoked. The returned object is nevertheless valid.
 * Use the method YModule.isOnline() to test if the module is
 * indeed online at a given time. In case of ambiguity when looking for
 * a module by logical name, no error is notified: the first instance
 * found is returned. The search is performed first by hardware name,
 * then by logical name.
 * 
 * @param func : a string containing either the serial number or
 *         the logical name of the desired module
 * 
 * @return a YModule object allowing you to drive the module
 *         or get additional information on the module.
 */
YModule* YModule::FindModule(string func)
{
    YModule* obj = NULL;
    obj = (YModule*) YFunction::_FindFromCache("Module", func);
    if (obj == NULL) {
        obj = new YModule(func);
        YFunction::_AddToCache("Module", func, obj);
    }
    return obj;
}

/**
 * Registers the callback function that is invoked on every change of advertised value.
 * The callback is invoked only during the execution of ySleep or yHandleEvents.
 * This provides control over the time when the callback is triggered. For good responsiveness, remember to call
 * one of these two functions periodically. To unregister a callback, pass a null pointer as argument.
 * 
 * @param callback : the callback function to call, or a null pointer. The callback function should take two
 *         arguments: the function object of which the value has changed, and the character string describing
 *         the new advertised value.
 * @noreturn
 */
int YModule::registerValueCallback(YModuleValueCallback callback)
{
    string val;
    if (callback != NULL) {
        YFunction::_UpdateValueCallbackList(this, true);
    } else {
        YFunction::_UpdateValueCallbackList(this, false);
    }
    _valueCallbackModule = callback;
    // Immediately invoke value callback with current value
    if (callback != NULL && this->isOnline()) {
        val = _advertisedValue;
        if (!(val == "")) {
            this->_invokeValueCallback(val);
        }
    }
    return 0;
}

int YModule::_invokeValueCallback(string value)
{
    if (_valueCallbackModule != NULL) {
        _valueCallbackModule(this, value);
    } else {
        YFunction::_invokeValueCallback(value);
    }
    return 0;
}

/**
 * Saves current settings in the nonvolatile memory of the module.
 * Warning: the number of allowed save operations during a module life is
 * limited (about 100000 cycles). Do not call this function within a loop.
 * 
 * @return YAPI_SUCCESS when the call succeeds.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
int YModule::saveToFlash(void)
{
    return this->set_persistentSettings(Y_PERSISTENTSETTINGS_SAVED);
}

/**
 * Reloads the settings stored in the nonvolatile memory, as
 * when the module is powered on.
 * 
 * @return YAPI_SUCCESS when the call succeeds.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
int YModule::revertFromFlash(void)
{
    return this->set_persistentSettings(Y_PERSISTENTSETTINGS_LOADED);
}

/**
 * Schedules a simple module reboot after the given number of seconds.
 * 
 * @param secBeforeReboot : number of seconds before rebooting
 * 
 * @return YAPI_SUCCESS when the call succeeds.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
int YModule::reboot(int secBeforeReboot)
{
    return this->set_rebootCountdown(secBeforeReboot);
}

/**
 * Schedules a module reboot into special firmware update mode.
 * 
 * @param secBeforeReboot : number of seconds before rebooting
 * 
 * @return YAPI_SUCCESS when the call succeeds.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
int YModule::triggerFirmwareUpdate(int secBeforeReboot)
{
    return this->set_rebootCountdown(-secBeforeReboot);
}

/**
 * Downloads the specified built-in file and returns a binary buffer with its content.
 * 
 * @param pathname : name of the new file to load
 * 
 * @return a binary buffer with the file content
 * 
 * On failure, throws an exception or returns an empty content.
 */
string YModule::download(string pathname)
{
    return this->_download(pathname);
}

/**
 * Returns the icon of the module. The icon is a PNG image and does not
 * exceeds 1536 bytes.
 * 
 * @return a binary buffer with module icon, in png format.
 */
string YModule::get_icon2d(void)
{
    return this->_download("icon2d.png");
}

/**
 * Returns a string with last logs of the module. This method return only
 * logs that are still in the module.
 * 
 * @return a string with last logs of the module.
 */
string YModule::get_lastLogs(void)
{
    string content;
    // may throw an exception
    content = this->_download("logs.txt");
    return content;
}

YModule *YModule::nextModule(void)
{
    string  hwid;
    
    if(YISERR(_nextFunction(hwid)) || hwid=="") {
        return NULL;
    }
    return YModule::FindModule(hwid);
}

YModule* YModule::FirstModule(void)
{
    vector<YFUN_DESCR>   v_fundescr;
    YDEV_DESCR             ydevice;
    string              serial, funcId, funcName, funcVal, errmsg;
    
    if(YISERR(YapiWrapper::getFunctionsByClass("Module", 0, v_fundescr, sizeof(YFUN_DESCR), errmsg)) ||
       v_fundescr.size() == 0 ||
       YISERR(YapiWrapper::getFunctionInfo(v_fundescr[0], ydevice, serial, funcId, funcName, funcVal, errmsg))) {
        return NULL;
    }
    return YModule::FindModule(serial+"."+funcId);
}

//--- (end of generated code: YModule implementation)

// Return a string that describes the function (class and logical name or hardware id)
string YModule::get_friendlyName(void)
{
    YFUN_DESCR   fundescr,moddescr;
    YDEV_DESCR   devdescr;
    string       errmsg, serial, funcId, funcName, funcValue;
    string       mod_serial, mod_funcId,mod_funcname;
    
    fundescr = YapiWrapper::getFunction(_className, _func, errmsg);
    if(!YISERR(fundescr) && !YISERR(YapiWrapper::getFunctionInfo(fundescr, devdescr, serial, funcId, funcName, funcValue, errmsg))) {
        moddescr = YapiWrapper::getFunction("Module", serial, errmsg);
        if(!YISERR(moddescr) && !YISERR(YapiWrapper::getFunctionInfo(moddescr, devdescr, mod_serial, mod_funcId, mod_funcname, funcValue, errmsg))) {
            if(mod_funcname!="") {
                return mod_funcname;
            }
        }
        return serial;
    }
    return Y_FRIENDLYNAME_INVALID;
}




void YModule::setImmutableAttributes(yDeviceSt *infos)
{
    _serialNumber = (string) infos->serial;
    _productName  = (string) infos->productname;
    _productId    =  infos->deviceid;
}


// Return the properties of the nth function of our device
YRETCODE YModule::_getFunction(int idx, string& serial, string& funcId, string& funcName, string& funcVal, string& errmsg)
{
    vector<YFUN_DESCR> *functions;
    YDevice     *dev;
    int         res;
    YFUN_DESCR   fundescr;
    YDEV_DESCR     devdescr;
    
    // retrieve device object
    res = _getDevice(dev, errmsg);
    if(YISERR(res)) {
        _throw((YRETCODE)res, errmsg);
        return (YRETCODE)res;
    }
    
    // get reference to all functions from the device
    res = dev->getFunctions(&functions, errmsg);
    if(YISERR(res)) return (YRETCODE)res;
    
    // get latest function info from yellow pages
    fundescr = functions->at(idx);
    res = YapiWrapper::getFunctionInfo(fundescr, devdescr, serial, funcId, funcName, funcVal, errmsg);
    if(YISERR(res)) return (YRETCODE)res;
    
    return YAPI_SUCCESS;
}

// Retrieve the number of functions (beside "module") in the device
int YModule::functionCount()
{
    vector<YFUN_DESCR> *functions;
    YDevice     *dev;
    string      errmsg;
    int         res;
    
    res = _getDevice(dev, errmsg);
    if(YISERR(res)) {
        _throw((YRETCODE)res, errmsg);
        return (YRETCODE)res;
    }
    res = dev->getFunctions(&functions, errmsg);
    if(YISERR(res)) {
        _throw((YRETCODE)res, errmsg);
        return (YRETCODE)res;
    }
    return (int)functions->size();
}

// Retrieve the Id of the nth function (beside "module") in the device
string YModule::functionId(int functionIndex)
{
    string      serial, funcId, funcName, funcVal, errmsg;
    
    int res = _getFunction(functionIndex, serial, funcId, funcName, funcVal, errmsg);
    if(YISERR(res)) {
        _throw((YRETCODE)res, errmsg);
        return YAPI_INVALID_STRING;
    }
    
    return funcId;
}  

// Retrieve the logical name of the nth function (beside "module") in the device
string YModule::functionName(int functionIndex)
{
    string      serial, funcId, funcName, funcVal, errmsg;
    
    int res = _getFunction(functionIndex, serial, funcId, funcName, funcVal, errmsg);
    if(YISERR(res)) {
        _throw((YRETCODE)res, errmsg);
        return YAPI_INVALID_STRING;
    }
    
    return funcName;
}

// Retrieve the advertised value of the nth function (beside "module") in the device
string YModule::functionValue(int functionIndex)
{
    string      serial, funcId, funcName, funcVal, errmsg;
    
    int res = _getFunction(functionIndex, serial, funcId, funcName, funcVal, errmsg);
    if(YISERR(res)) {
        _throw((YRETCODE)res, errmsg);
        return YAPI_INVALID_STRING;
    }    
    
    return funcVal;
}

/**
 * Registers a device log callback function. This callback will be called each time
 * that a module sends a new log message. Mostly useful to debug a Yoctopuce module.
 * 
 * @param callback : the callback function to call, or a null pointer. The callback function should take two
 *         arguments: the module object that emitted the log message, and the character string containing the log.
 * @noreturn
 */
void YModule::registerLogCallback(YModuleLogCallback callback)
{
    _logCallback = callback;
    yapiStartStopDeviceLogCallback(_serial.c_str(), _logCallback!=NULL);
}

YModuleLogCallback YModule::get_logCallback()
{
    return _logCallback;
}


//--- (generated code: Module functions)
//--- (end of generated code: Module functions)




YSensor::YSensor(const string& func): YFunction(func)
//--- (generated code: Sensor initialization)
    ,_unit(UNIT_INVALID)
    ,_currentValue(CURRENTVALUE_INVALID)
    ,_lowestValue(LOWESTVALUE_INVALID)
    ,_highestValue(HIGHESTVALUE_INVALID)
    ,_currentRawValue(CURRENTRAWVALUE_INVALID)
    ,_logFrequency(LOGFREQUENCY_INVALID)
    ,_reportFrequency(REPORTFREQUENCY_INVALID)
    ,_calibrationParam(CALIBRATIONPARAM_INVALID)
    ,_resolution(RESOLUTION_INVALID)
    ,_valueCallbackSensor(NULL)
    ,_timedReportCallbackSensor(NULL)
    ,_prevTimedReport(0.0)
    ,_iresol(0.0)
    ,_offset(0.0)
    ,_scale(0.0)
    ,_decexp(0.0)
    ,_caltyp(0)
//--- (end of generated code: Sensor initialization)
{
    _className = "Sensor";
}

YSensor::~YSensor() 
{
//--- (generated code: YSensor cleanup)
//--- (end of generated code: YSensor cleanup)
}


// Method used to encode calibration points into fixed-point 16-bit integers or decimal floating-point
//
string YSensor::_encodeCalibrationPoints(const floatArr& rawValues, const floatArr& refValues, const string& actualCparams)
{
    int     npt = (int)(rawValues.size() < refValues.size() ? rawValues.size() : refValues.size());
    int     caltype;
    int     rawVal, refVal;
    int     minRaw = 0;
    char    buff[32];
    string  res;
    
    if(npt == 0){
        return "0";
    }

    int pos = (int)actualCparams.find(',');
    if(actualCparams=="" || actualCparams=="0" || pos>=0) {
        _throw(YAPI_NOT_SUPPORTED, "Device does not support new calibration parameters. Please upgrade your firmware");
        return "0";
    }
    vector<int> iCalib = YAPI::_decodeWords(actualCparams);
    int calibrationOffset = iCalib[0];
    int divisor = iCalib[1];
    if(divisor > 0) {
        caltype = npt;
    } else {
        caltype = 10+npt;
    }
    sprintf(buff, "%u", caltype);
    res = string(buff);
    if (caltype <=10){
        for(int i = 0; i < npt; i++) {
            rawVal = (int) (rawValues[i] * divisor - calibrationOffset + .5);
            if(rawVal >= minRaw && rawVal < 65536) {
                refVal = (int) (refValues[i] * divisor - calibrationOffset + .5);
                if(refVal >= 0 && refVal < 65536) {
                    sprintf(buff, ",%d,%d", rawVal, refVal);
                    res += string(buff);
                    minRaw = rawVal+1;
                }
            }
        }
    } else {
        // 16-bit floating-point decimal encoding
        for(int i = 0; i < npt; i++) {
            rawVal = YAPI::_doubleToDecimal(rawValues[i]);
            refVal = YAPI::_doubleToDecimal(refValues[i]);
            sprintf(buff, ",%d,%d", rawVal, refVal);
            res += string(buff);
        }
    }
    return res;
}


int YSensor::_decodeCalibrationPoints(const string& calibParams,intArr& intPt, floatArr& rawPt, floatArr& calPt)
{    
    int         calibType, rawval, calval;
    unsigned    pos=0;
    vector <int> iCalib;
    double calibrationOffset,divisor;

    intPt.clear();
    rawPt.clear();
    calPt.clear();
    if (calibParams== "" || calibParams== "0") {
        // old format: no calibration
        return 0;
    }
    if(calibParams.find(',') != string::npos) {
        // old format -> device must do the calibration
        return -1;
    }

    // new format
    iCalib = YAPI::_decodeWords(calibParams);
    if(iCalib.size() < 2) {
        // bad format
        return -1; 
    }
    if(iCalib.size() == 2) {
        // no calibration
        return 0;
    }
    calibrationOffset = iCalib[pos++];
    divisor = iCalib[pos++];
    calibType = iCalib[pos++];
    
    // parse calibration parameters
    while(pos+1 < iCalib.size()) {
        rawval = iCalib[pos++];
        calval = iCalib[pos++];
        intPt.push_back(rawval);
        intPt.push_back(calval);
        if(divisor != 0) {
            rawPt.push_back((rawval + calibrationOffset) / divisor);
            calPt.push_back((calval + calibrationOffset) / divisor);
        } else {
            rawPt.push_back(YAPI::_decimalToDouble(rawval));
            calPt.push_back(YAPI::_decimalToDouble(calval));
        }
    }
    if (intPt.size() < 10) {
        return -1;
    }       
    return calibType;
}


//--- (generated code: YSensor implementation)
// static attributes
const string YSensor::UNIT_INVALID = YAPI_INVALID_STRING;
const double YSensor::CURRENTVALUE_INVALID = YAPI_INVALID_DOUBLE;
const double YSensor::LOWESTVALUE_INVALID = YAPI_INVALID_DOUBLE;
const double YSensor::HIGHESTVALUE_INVALID = YAPI_INVALID_DOUBLE;
const double YSensor::CURRENTRAWVALUE_INVALID = YAPI_INVALID_DOUBLE;
const string YSensor::LOGFREQUENCY_INVALID = YAPI_INVALID_STRING;
const string YSensor::REPORTFREQUENCY_INVALID = YAPI_INVALID_STRING;
const string YSensor::CALIBRATIONPARAM_INVALID = YAPI_INVALID_STRING;
const double YSensor::RESOLUTION_INVALID = YAPI_INVALID_DOUBLE;

int YSensor::_parseAttr(yJsonStateMachine& j)
{
    if(!strcmp(j.token, "unit")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _unit =  _parseString(j);
        return 1;
    }
    if(!strcmp(j.token, "currentValue")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _currentValue =  atof(j.token)/65536;
        return 1;
    }
    if(!strcmp(j.token, "lowestValue")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _lowestValue =  atof(j.token)/65536;
        return 1;
    }
    if(!strcmp(j.token, "highestValue")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _highestValue =  atof(j.token)/65536;
        return 1;
    }
    if(!strcmp(j.token, "currentRawValue")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _currentRawValue =  atof(j.token)/65536;
        return 1;
    }
    if(!strcmp(j.token, "logFrequency")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _logFrequency =  _parseString(j);
        return 1;
    }
    if(!strcmp(j.token, "reportFrequency")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _reportFrequency =  _parseString(j);
        return 1;
    }
    if(!strcmp(j.token, "calibrationParam")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _calibrationParam =  _parseString(j);
        return 1;
    }
    if(!strcmp(j.token, "resolution")) {
        if(yJsonParse(&j) != YJSON_PARSE_AVAIL) goto failed;
        _resolution =  (atoi(j.token) > 100 ? 1.0 / floor(65536.0/atof(j.token)+.5) : 0.001 / floor(67.0/atof(j.token)+.5));
        return 1;
    }
    failed:
    return YFunction::_parseAttr(j);
}


/**
 * Returns the measuring unit for the measure.
 * 
 * @return a string corresponding to the measuring unit for the measure
 * 
 * On failure, throws an exception or returns Y_UNIT_INVALID.
 */
string YSensor::get_unit(void)
{
    if (_cacheExpiration <= YAPI::GetTickCount()) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YSensor::UNIT_INVALID;
        }
    }
    return _unit;
}

/**
 * Returns the current value of the measure.
 * 
 * @return a floating point number corresponding to the current value of the measure
 * 
 * On failure, throws an exception or returns Y_CURRENTVALUE_INVALID.
 */
double YSensor::get_currentValue(void)
{
    double res = 0.0;
    if (_cacheExpiration <= YAPI::GetTickCount()) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YSensor::CURRENTVALUE_INVALID;
        }
    }
    res = this->_applyCalibration(_currentRawValue);
    if (res == YSensor::CURRENTVALUE_INVALID) {
        res = _currentValue;
    }
    res = res * _iresol;
    return (res < 0.0 ? ceil(res-0.5) : floor(res+0.5)) / _iresol;
}

/**
 * Changes the recorded minimal value observed.
 * 
 * @param newval : a floating point number corresponding to the recorded minimal value observed
 * 
 * @return YAPI_SUCCESS if the call succeeds.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
int YSensor::set_lowestValue(double newval)
{
    string rest_val;
    char buf[32]; sprintf(buf,"%d", (int)floor(newval*65536.0 +0.5)); rest_val = string(buf);
    return _setAttr("lowestValue", rest_val);
}

/**
 * Returns the minimal value observed for the measure since the device was started.
 * 
 * @return a floating point number corresponding to the minimal value observed for the measure since
 * the device was started
 * 
 * On failure, throws an exception or returns Y_LOWESTVALUE_INVALID.
 */
double YSensor::get_lowestValue(void)
{
    double res = 0.0;
    if (_cacheExpiration <= YAPI::GetTickCount()) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YSensor::LOWESTVALUE_INVALID;
        }
    }
    res = _lowestValue * _iresol;
    return (res < 0.0 ? ceil(res-0.5) : floor(res+0.5)) / _iresol;
}

/**
 * Changes the recorded maximal value observed.
 * 
 * @param newval : a floating point number corresponding to the recorded maximal value observed
 * 
 * @return YAPI_SUCCESS if the call succeeds.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
int YSensor::set_highestValue(double newval)
{
    string rest_val;
    char buf[32]; sprintf(buf,"%d", (int)floor(newval*65536.0 +0.5)); rest_val = string(buf);
    return _setAttr("highestValue", rest_val);
}

/**
 * Returns the maximal value observed for the measure since the device was started.
 * 
 * @return a floating point number corresponding to the maximal value observed for the measure since
 * the device was started
 * 
 * On failure, throws an exception or returns Y_HIGHESTVALUE_INVALID.
 */
double YSensor::get_highestValue(void)
{
    double res = 0.0;
    if (_cacheExpiration <= YAPI::GetTickCount()) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YSensor::HIGHESTVALUE_INVALID;
        }
    }
    res = _highestValue * _iresol;
    return (res < 0.0 ? ceil(res-0.5) : floor(res+0.5)) / _iresol;
}

/**
 * Returns the uncalibrated, unrounded raw value returned by the sensor.
 * 
 * @return a floating point number corresponding to the uncalibrated, unrounded raw value returned by the sensor
 * 
 * On failure, throws an exception or returns Y_CURRENTRAWVALUE_INVALID.
 */
double YSensor::get_currentRawValue(void)
{
    if (_cacheExpiration <= YAPI::GetTickCount()) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YSensor::CURRENTRAWVALUE_INVALID;
        }
    }
    return _currentRawValue;
}

/**
 * Returns the datalogger recording frequency for this function, or "OFF"
 * when measures are not stored in the data logger flash memory.
 * 
 * @return a string corresponding to the datalogger recording frequency for this function, or "OFF"
 *         when measures are not stored in the data logger flash memory
 * 
 * On failure, throws an exception or returns Y_LOGFREQUENCY_INVALID.
 */
string YSensor::get_logFrequency(void)
{
    if (_cacheExpiration <= YAPI::GetTickCount()) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YSensor::LOGFREQUENCY_INVALID;
        }
    }
    return _logFrequency;
}

/**
 * Changes the datalogger recording frequency for this function.
 * The frequency can be specified as samples per second,
 * as sample per minute (for instance "15/m") or in samples per
 * hour (eg. "4/h"). To disable recording for this function, use
 * the value "OFF".
 * 
 * @param newval : a string corresponding to the datalogger recording frequency for this function
 * 
 * @return YAPI_SUCCESS if the call succeeds.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
int YSensor::set_logFrequency(const string& newval)
{
    string rest_val;
    rest_val = newval;
    return _setAttr("logFrequency", rest_val);
}

/**
 * Returns the timed value notification frequency, or "OFF" if timed
 * value notifications are disabled for this function.
 * 
 * @return a string corresponding to the timed value notification frequency, or "OFF" if timed
 *         value notifications are disabled for this function
 * 
 * On failure, throws an exception or returns Y_REPORTFREQUENCY_INVALID.
 */
string YSensor::get_reportFrequency(void)
{
    if (_cacheExpiration <= YAPI::GetTickCount()) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YSensor::REPORTFREQUENCY_INVALID;
        }
    }
    return _reportFrequency;
}

/**
 * Changes the timed value notification frequency for this function.
 * The frequency can be specified as samples per second,
 * as sample per minute (for instance "15/m") or in samples per
 * hour (eg. "4/h"). To disable timed value notifications for this
 * function, use the value "OFF".
 * 
 * @param newval : a string corresponding to the timed value notification frequency for this function
 * 
 * @return YAPI_SUCCESS if the call succeeds.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
int YSensor::set_reportFrequency(const string& newval)
{
    string rest_val;
    rest_val = newval;
    return _setAttr("reportFrequency", rest_val);
}

string YSensor::get_calibrationParam(void)
{
    if (_cacheExpiration <= YAPI::GetTickCount()) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YSensor::CALIBRATIONPARAM_INVALID;
        }
    }
    return _calibrationParam;
}

int YSensor::set_calibrationParam(const string& newval)
{
    string rest_val;
    rest_val = newval;
    return _setAttr("calibrationParam", rest_val);
}

/**
 * Changes the resolution of the measured physical values. The resolution corresponds to the numerical precision
 * when displaying value. It does not change the precision of the measure itself.
 * 
 * @param newval : a floating point number corresponding to the resolution of the measured physical values
 * 
 * @return YAPI_SUCCESS if the call succeeds.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
int YSensor::set_resolution(double newval)
{
    string rest_val;
    char buf[32]; sprintf(buf,"%d", (int)floor(newval*65536.0 +0.5)); rest_val = string(buf);
    return _setAttr("resolution", rest_val);
}

/**
 * Returns the resolution of the measured values. The resolution corresponds to the numerical precision
 * of the measures, which is not always the same as the actual precision of the sensor.
 * 
 * @return a floating point number corresponding to the resolution of the measured values
 * 
 * On failure, throws an exception or returns Y_RESOLUTION_INVALID.
 */
double YSensor::get_resolution(void)
{
    if (_cacheExpiration <= YAPI::GetTickCount()) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YSensor::RESOLUTION_INVALID;
        }
    }
    return _resolution;
}

/**
 * Retrieves a sensor for a given identifier.
 * The identifier can be specified using several formats:
 * <ul>
 * <li>FunctionLogicalName</li>
 * <li>ModuleSerialNumber.FunctionIdentifier</li>
 * <li>ModuleSerialNumber.FunctionLogicalName</li>
 * <li>ModuleLogicalName.FunctionIdentifier</li>
 * <li>ModuleLogicalName.FunctionLogicalName</li>
 * </ul>
 * 
 * This function does not require that the sensor is online at the time
 * it is invoked. The returned object is nevertheless valid.
 * Use the method YSensor.isOnline() to test if the sensor is
 * indeed online at a given time. In case of ambiguity when looking for
 * a sensor by logical name, no error is notified: the first instance
 * found is returned. The search is performed first by hardware name,
 * then by logical name.
 * 
 * @param func : a string that uniquely characterizes the sensor
 * 
 * @return a YSensor object allowing you to drive the sensor.
 */
YSensor* YSensor::FindSensor(string func)
{
    YSensor* obj = NULL;
    obj = (YSensor*) YFunction::_FindFromCache("Sensor", func);
    if (obj == NULL) {
        obj = new YSensor(func);
        YFunction::_AddToCache("Sensor", func, obj);
    }
    return obj;
}

/**
 * Registers the callback function that is invoked on every change of advertised value.
 * The callback is invoked only during the execution of ySleep or yHandleEvents.
 * This provides control over the time when the callback is triggered. For good responsiveness, remember to call
 * one of these two functions periodically. To unregister a callback, pass a null pointer as argument.
 * 
 * @param callback : the callback function to call, or a null pointer. The callback function should take two
 *         arguments: the function object of which the value has changed, and the character string describing
 *         the new advertised value.
 * @noreturn
 */
int YSensor::registerValueCallback(YSensorValueCallback callback)
{
    string val;
    if (callback != NULL) {
        YFunction::_UpdateValueCallbackList(this, true);
    } else {
        YFunction::_UpdateValueCallbackList(this, false);
    }
    _valueCallbackSensor = callback;
    // Immediately invoke value callback with current value
    if (callback != NULL && this->isOnline()) {
        val = _advertisedValue;
        if (!(val == "")) {
            this->_invokeValueCallback(val);
        }
    }
    return 0;
}

int YSensor::_invokeValueCallback(string value)
{
    if (_valueCallbackSensor != NULL) {
        _valueCallbackSensor(this, value);
    } else {
        YFunction::_invokeValueCallback(value);
    }
    return 0;
}

int YSensor::_parserHelper(void)
{
    int position = 0;
    int maxpos = 0;
    vector<int> iCalib;
    int iRaw = 0;
    int iRef = 0;
    double fRaw = 0.0;
    double fRef = 0.0;
    // Store inverted resolution, to provide better rounding
    if (_resolution > 0) {
        _iresol = (1.0 / _resolution < 0.0 ? ceil(1.0 / _resolution-0.5) : floor(1.0 / _resolution+0.5));
    } else {
        return 0;
    }
    
    _scale = -1;
    _calpar.clear();
    _calraw.clear();
    _calref.clear();
    
    // Old format: supported when there is no calibration
    if (_calibrationParam == "" || _calibrationParam == "0") {
        _caltyp = 0;
        return 0;
    }
    // Old format: calibrated value will be provided by the device
    if (_ystrpos(_calibrationParam, ",") >= 0) {
        _caltyp = -1;
        return 0;
    }
    // New format, decode parameters
    iCalib = YAPI::_decodeWords(_calibrationParam);
    // In case of unknown format, calibrated value will be provided by the device
    if ((int)iCalib.size() < 2) {
        _caltyp = -1;
        return 0;
    }
    
    // Save variable format (scale for scalar, or decimal exponent)
    _isScal = (iCalib[1] > 0);
    if (_isScal) {
        _offset = iCalib[0];
        if (_offset > 32767) {
            _offset = _offset - 65536;
        }
        _scale = iCalib[1];
        _decexp = 0;
    } else {
        _offset = 0;
        _scale = 1;
        _decexp = 1.0;
        position = iCalib[0];
        while (position > 0) {
            _decexp = _decexp * 10;
            position = position - 1;
        }
    }
    
    // Shortcut when there is no calibration parameter
    if ((int)iCalib.size() == 2) {
        _caltyp = 0;
        return 0;
    }
    
    _caltyp = iCalib[2];
    _calhdl = YAPI::_getCalibrationHandler(_caltyp);
    // parse calibration points
    position = 3;
    if (_caltyp <= 10) {
        maxpos = _caltyp;
    } else {
        if (_caltyp <= 20) {
            maxpos = _caltyp - 10;
        } else {
            maxpos = 5;
        }
    }
    maxpos = 3 + 2 * maxpos;
    if (maxpos > (int)iCalib.size()) {
        maxpos = (int)iCalib.size();
    }
    _calpar.clear();
    _calraw.clear();
    _calref.clear();
    while (position + 1 < maxpos) {
        iRaw = iCalib[position];
        iRef = iCalib[position + 1];
        _calpar.push_back(iRaw);
        _calpar.push_back(iRef);
        if (_isScal) {
            fRaw = iRaw;
            fRaw = (fRaw - _offset) / _scale;
            fRef = iRef;
            fRef = (fRef - _offset) / _scale;
            _calraw.push_back(fRaw);
            _calref.push_back(fRef);
        } else {
            _calraw.push_back(YAPI::_decimalToDouble(iRaw));
            _calref.push_back(YAPI::_decimalToDouble(iRef));
        }
        position = position + 2;
    }
    
    
    
    return 0;
}

/**
 * Retrieves a DataSet object holding historical data for this
 * sensor, for a specified time interval. The measures will be
 * retrieved from the data logger, which must have been turned
 * on at the desired time. See the documentation of the DataSet
 * class for information on how to get an overview of the
 * recorded data, and how to load progressively a large set
 * of measures from the data logger.
 * 
 * This function only works if the device uses a recent firmware,
 * as DataSet objects are not supported by firmwares older than
 * version 13000.
 * 
 * @param startTime : the start of the desired measure time interval,
 *         as a Unix timestamp, i.e. the number of seconds since
 *         January 1, 1970 UTC. The special value 0 can be used
 *         to include any meaasure, without initial limit.
 * @param endTime : the end of the desired measure time interval,
 *         as a Unix timestamp, i.e. the number of seconds since
 *         January 1, 1970 UTC. The special value 0 can be used
 *         to include any meaasure, without ending limit.
 * 
 * @return an instance of YDataSet, providing access to historical
 *         data. Past measures can be loaded progressively
 *         using methods from the YDataSet object.
 */
YDataSet YSensor::get_recordedData(s64 startTime,s64 endTime)
{
    string funcid;
    string funit;
    // may throw an exception
    funcid = this->get_functionId();
    funit = this->get_unit();
    return YDataSet(this,funcid,funit,startTime,endTime);
}

/**
 * Registers the callback function that is invoked on every periodic timed notification.
 * The callback is invoked only during the execution of ySleep or yHandleEvents.
 * This provides control over the time when the callback is triggered. For good responsiveness, remember to call
 * one of these two functions periodically. To unregister a callback, pass a null pointer as argument.
 * 
 * @param callback : the callback function to call, or a null pointer. The callback function should take two
 *         arguments: the function object of which the value has changed, and an YMeasure object describing
 *         the new advertised value.
 * @noreturn
 */
int YSensor::registerTimedReportCallback(YSensorTimedReportCallback callback)
{
    if (callback != NULL) {
        YFunction::_UpdateTimedReportCallbackList(this, true);
    } else {
        YFunction::_UpdateTimedReportCallbackList(this, false);
    }
    _timedReportCallbackSensor = callback;
    return 0;
}

int YSensor::_invokeTimedReportCallback(YMeasure value)
{
    if (_timedReportCallbackSensor != NULL) {
        _timedReportCallbackSensor(this, value);
    } else {
    }
    return 0;
}

/**
 * Configures error correction data points, in particular to compensate for
 * a possible perturbation of the measure caused by an enclosure. It is possible
 * to configure up to five correction points. Correction points must be provided
 * in ascending order, and be in the range of the sensor. The device will automatically
 * perform a linear interpolation of the error correction between specified
 * points. Remember to call the saveToFlash() method of the module if the
 * modification must be kept.
 * 
 * For more information on advanced capabilities to refine the calibration of
 * sensors, please contact support@yoctopuce.com.
 * 
 * @param rawValues : array of floating point numbers, corresponding to the raw
 *         values returned by the sensor for the correction points.
 * @param refValues : array of floating point numbers, corresponding to the corrected
 *         values for the correction points.
 * 
 * @return YAPI_SUCCESS if the call succeeds.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
int YSensor::calibrateFromPoints(vector<double> rawValues,vector<double> refValues)
{
    string rest_val;
    // may throw an exception
    rest_val = this->_encodeCalibrationPoints(rawValues, refValues);
    return this->_setAttr("calibrationParam", rest_val);
}

/**
 * Retrieves error correction data points previously entered using the method
 * calibrateFromPoints.
 * 
 * @param rawValues : array of floating point numbers, that will be filled by the
 *         function with the raw sensor values for the correction points.
 * @param refValues : array of floating point numbers, that will be filled by the
 *         function with the desired values for the correction points.
 * 
 * @return YAPI_SUCCESS if the call succeeds.
 * 
 * On failure, throws an exception or returns a negative error code.
 */
int YSensor::loadCalibrationPoints(vector<double>& rawValues,vector<double>& refValues)
{
    rawValues.clear();
    refValues.clear();
    
    // Load function parameters if not yet loaded
    if (_scale == 0) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YAPI_DEVICE_NOT_FOUND;
        }
    }
    
    if (_caltyp < 0) {
        this->_throw(YAPI_NOT_SUPPORTED, "Device does not support new calibration parameters. Please upgrade your firmware");
        return YAPI_NOT_SUPPORTED;
    }
    rawValues.clear();
    refValues.clear();
    for (unsigned ii = 0; ii < _calraw.size(); ii++) {
        rawValues.push_back(_calraw[ii]);
    }
    for (unsigned ii = 0; ii < _calref.size(); ii++) {
        refValues.push_back(_calref[ii]);
    }
    return YAPI_SUCCESS;
}

string YSensor::_encodeCalibrationPoints(vector<double> rawValues,vector<double> refValues)
{
    string res;
    int npt = 0;
    int idx = 0;
    int iRaw = 0;
    int iRef = 0;
    
    npt = (int)rawValues.size();
    if (npt != (int)refValues.size()) {
        this->_throw(YAPI_INVALID_ARGUMENT, "Invalid calibration parameters (size mismatch)");
        return YAPI_INVALID_STRING;
    }
    
    // Shortcut when building empty calibration parameters
    if (npt == 0) {
        return "0";
    }
    
    // Load function parameters if not yet loaded
    if (_scale == 0) {
        if (this->load(YAPI::DefaultCacheValidity) != YAPI_SUCCESS) {
            return YAPI_INVALID_STRING;
        }
    }
    
    // Detect old firmware
    if ((_caltyp < 0) || (_scale < 0)) {
        this->_throw(YAPI_NOT_SUPPORTED, "Device does not support new calibration parameters. Please upgrade your firmware");
        return "0";
    }
    if (_isScal) {
        res = YapiWrapper::ysprintf("%d",npt);
        idx = 0;
        while (idx < npt) {
            iRaw = (int) (rawValues[idx] * _scale + _offset < 0.0 ? ceil(rawValues[idx] * _scale + _offset-0.5) : floor(rawValues[idx] * _scale + _offset+0.5));
            iRef = (int) (refValues[idx] * _scale + _offset < 0.0 ? ceil(refValues[idx] * _scale + _offset-0.5) : floor(refValues[idx] * _scale + _offset+0.5));
            res = YapiWrapper::ysprintf("%s,%d,%d", res.c_str(), iRaw,iRef);
            idx = idx + 1;
        }
    } else {
        res = YapiWrapper::ysprintf("%d",10 + npt);
        idx = 0;
        while (idx < npt) {
            iRaw = (int) YAPI::_doubleToDecimal(rawValues[idx]);
            iRef = (int) YAPI::_doubleToDecimal(refValues[idx]);
            res = YapiWrapper::ysprintf("%s,%d,%d", res.c_str(), iRaw,iRef);
            idx = idx + 1;
        }
    }
    return res;
}

double YSensor::_applyCalibration(double rawValue)
{
    if (rawValue == Y_CURRENTVALUE_INVALID) {
        return Y_CURRENTVALUE_INVALID;
    }
    if (_caltyp == 0) {
        return rawValue;
    }
    if (_caltyp < 0) {
        return Y_CURRENTVALUE_INVALID;
    }
    if (!(_calhdl != NULL)) {
        return Y_CURRENTVALUE_INVALID;
    }
    return _calhdl(rawValue, _caltyp, _calpar, _calraw, _calref);
}

YMeasure YSensor::_decodeTimedReport(double timestamp,vector<int> report)
{
    int i = 0;
    int byteVal = 0;
    int poww = 0;
    int minRaw = 0;
    int avgRaw = 0;
    int maxRaw = 0;
    double startTime = 0.0;
    double endTime = 0.0;
    double minVal = 0.0;
    double avgVal = 0.0;
    double maxVal = 0.0;
    
    startTime = _prevTimedReport;
    endTime = timestamp;
    _prevTimedReport = endTime;
    if (startTime == 0) {
        startTime = endTime;
    }
    if (report[0] > 0) {
        minRaw = report[1] + 0x100 * report[2];
        maxRaw = report[3] + 0x100 * report[4];
        avgRaw = report[5] + 0x100 * report[6] + 0x10000 * report[7];
        byteVal = report[8];
        if (((byteVal) & (0x80)) == 0) {
            avgRaw = avgRaw + 0x1000000 * byteVal;
        } else {
            avgRaw = avgRaw - 0x1000000 * (0x100 - byteVal);
        }
        minVal = this->_decodeVal(minRaw);
        avgVal = this->_decodeAvg(avgRaw);
        maxVal = this->_decodeVal(maxRaw);
    } else {
        poww = 1;
        avgRaw = 0;
        byteVal = 0;
        i = 1;
        while (i < (int)report.size()) {
            byteVal = report[i];
            avgRaw = avgRaw + poww * byteVal;
            poww = poww * 0x100;
            i = i + 1;
        }
        if (_isScal) {
            avgVal = this->_decodeVal(avgRaw);
        } else {
            if (((byteVal) & (0x80)) != 0) {
                avgRaw = avgRaw - poww;
            }
            avgVal = this->_decodeAvg(avgRaw);
        }
        minVal = avgVal;
        maxVal = avgVal;
    }
    
    return YMeasure( startTime, endTime, minVal, avgVal,maxVal);
}

double YSensor::_decodeVal(int w)
{
    double val = 0.0;
    val = w;
    if (_isScal) {
        val = (val - _offset) / _scale;
    } else {
        val = YAPI::_decimalToDouble(w);
    }
    if (_caltyp != 0) {
        val = _calhdl(val, _caltyp, _calpar, _calraw, _calref);
    }
    return val;
}

double YSensor::_decodeAvg(int dw)
{
    double val = 0.0;
    val = dw;
    if (_isScal) {
        val = (val / 100 - _offset) / _scale;
    } else {
        val = val / _decexp;
    }
    if (_caltyp != 0) {
        val = _calhdl(val, _caltyp, _calpar, _calraw, _calref);
    }
    return val;
}

YSensor *YSensor::nextSensor(void)
{
    string  hwid;
    
    if(YISERR(_nextFunction(hwid)) || hwid=="") {
        return NULL;
    }
    return YSensor::FindSensor(hwid);
}

YSensor* YSensor::FirstSensor(void)
{
    vector<YFUN_DESCR>   v_fundescr;
    YDEV_DESCR             ydevice;
    string              serial, funcId, funcName, funcVal, errmsg;
    
    if(YISERR(YapiWrapper::getFunctionsByClass("Sensor", 0, v_fundescr, sizeof(YFUN_DESCR), errmsg)) ||
       v_fundescr.size() == 0 ||
       YISERR(YapiWrapper::getFunctionInfo(v_fundescr[0], ydevice, serial, funcId, funcName, funcVal, errmsg))) {
        return NULL;
    }
    return YSensor::FindSensor(serial+"."+funcId);
}

//--- (end of generated code: YSensor implementation)

//--- (generated code: Sensor functions)
//--- (end of generated code: Sensor functions)
