from .al import *
from .alc import *

import ctypes, warnings

import os, sys

try:
    from pyogg import *
    PYOGG_AVAIL = (PYOGG_OGG_AVAIL and PYOGG_VORBIS_AVAIL and PYOGG_VORBIS_FILE_AVAIL) or (PYOGG_OPUS_AVAIL and PYOGG_OPUS_FILE_AVAIL) or PYOGG_FLAC_AVAIL
except:
    PYOGG_AVAIL = False

try:
    import wave
    WAVE_AVAIL = True
except:
    WAVE_AVAIL = False

try:
    long
except:
    long = int

tuple_add3 = lambda a,b: (a[0]+b[0], a[1]+b[1], a[2]+b[2])

OAL_DONT_AUTO_INIT = False

OAL_STREAM_BUFFER_COUNT = 2

WAVE_STREAM_BUFFER_SIZE = 4096*2

MAX_FLOAT = sys.float_info.max

ALboolean = ctypes.c_bool
ALchar = ctypes.c_char
ALbyte = ctypes.c_byte
ALubyte = ctypes.c_ubyte
ALshort = ctypes.c_int16
ALushort = ctypes.c_uint16
ALint = ctypes.c_int32
ALuint = ctypes.c_uint32
ALsizei = ctypes.c_int32
ALenum = ctypes.c_int32
ALfloat = ctypes.c_float
ALdouble = ctypes.c_double

_sources = []
_buffers = []

class OalError(Exception):
    pass

class OalWarning(Warning):
    pass

def _err(msg):
    raise OalError(msg)

_oaldevice = None

_oalcontext = None

def oalInit(device_specifier=None, context_attr_list=None):
    """oalInit([c_char_p device_specifier = None, POINTER(c_int) context_attr_list = None]) -> None
    Sets up PyOpenAL's device and context (if not yet created)"""
    global _oaldevice, _oalcontext
    
    if not _oaldevice:
        _oaldevice = alcOpenDevice(device_specifier)
        
    if not _oaldevice:
        _err("Default OpenAL device couldn't be opened")

    if not _oalcontext:
        _oalcontext = alcCreateContext(_oaldevice, context_attr_list)
        
    if not _oalcontext:
        _err("OpenAL context couldn't be created")

    alcMakeContextCurrent(_oalcontext)

def oalGetDevice():
    """oalGetDevice() -> ALCdevice
    returns the OpenAL device PyOpenAL is using"""
    global _oaldevice
    return _oaldevice

def oalGetContext():
    """oalGetContext() -> ALCcontext
    returns the OpenAL context PyOpenAL is using"""
    global _oalcontext
    return _oalcontext

def oalGetInit():
    """oalGetInit() -> bool
    finds out if PyOpenAL is initialized"""
    global _oaldevice, _oalcontext

    return bool(_oaldevice) and bool(_oalcontext)

def _alError():
    _err("PyOpenAL wasn't loaded yet, please run oalInit() first")

def _check():
    if not oalGetInit():
        if OAL_DONT_AUTO_INIT:
            _alError()
        else:
            oalInit()

def _no_pyogg_error(*args, **kw):
    _err("You have to set up pyogg in order to use this function. Go to https://github.com/Zuzu-Typ/PyOgg to get it")

def _to_val(value):
    if type(value) in (float, bool, tuple):
        return value
    elif type(value) == int:
        return float(value)
    elif type(value) == ctypes.c_float*3:
        return tuple(value)
    elif type(value) == ctypes.c_float*6:
        return tuple(value)
    elif type(value) in (ctypes.c_float, ctypes.c_int, ctypes.c_uint):
        return value.value
    else:
        try:
            if len(value) == 3:
                return tuple(value)
            elif len(value) == 6:
                return tuple(value)
        except: pass

if WAVE_AVAIL:
    class WaveFile:
        def __init__(self,path):
            self.file_ = wave.open(path)

            self.channels = self.file_.getnchannels()

            self.frequency = self.file_.getframerate()
            
            temp_buffer = []

            change = 1

            while change:
                new_buf = self.file_.readframes(4096*8)
                change = len(new_buf)
                temp_buffer.append(new_buf)
            self.buffer = b"".join(temp_buffer)

            self.buffer_length = len(self.buffer)

            self.file_.close()

    class WaveFileStream:
        def __init__(self, path):
            self.file_ = wave.open(path)

            self.channels = self.file_.getnchannels()

            self.frequency = self.file_.getframerate()

            self.exists = True

        def clean_up(self):
            self.file_.close()

            self.exists = False

        def get_buffer(self):
            """get_buffer() -> bytesBuffer, bufferLength"""
            if not self.exists:
                return None
            buffer = []
            buffer_size = 0
            
            while True:
                new_bytes = self.file_.readframes(WAVE_STREAM_BUFFER_SIZE*self.channels)
                
                buffer.append(new_bytes)
                buffer_size += len(new_bytes)

                if len(new_bytes) == 0 or buffer_size >= WAVE_STREAM_BUFFER_SIZE*self.channels:
                    break

            if len(buffer) == 0:
                self.clean_up()
                return(None)

            buffer_bytes = b"".join(buffer)
            return(buffer_bytes, len(buffer_bytes))
else:
    class WaveFile:
        def __init__(*args, **kw):
            _err("Wave seems to be unavailable (maybe your Python version doesn't support it...?")
        
class Listener:
    """An interface to the OpenAL Listener (you)
    An instance of this class is created autometically,
    you can retrieve it with oalGetListener()"""
    def __init__(self):
        self.gain = 0.
        self.position = (0.,0.,0.)
        self.orientation = (0.,0.,-1.,0.,1.,0.)
        self.velocity = (0.,0.,0.)

    def get(self, enum):
        """get(int enum) -> value
        tries to get <enum> (e.g. AL_GAIN) for the Listener.
        you can also use the instance variables (e.g. Listener.gain)
        (note that instance variables are only updated when calling the
         respective set_ functions (e.g. set_gain))"""
        if enum in (AL_GAIN,):
            value = ctypes.c_float()
            alGetListenerf(enum, value)

        elif enum in (AL_POSITION,
                      AL_VELOCITY):
            value = (ctypes.c_float * 3)()
            alGetListenerfv( enum, value)

        elif enum in (AL_ORIENTATION,):
            value = (ctypes.c_float * 6)()
            alGetListenerfv( enum, value)

        else:
            _err("cannot get({}), this enum doesn't exist or can't be grabbed".format(enum))

        return _to_val(value)

    def set(self, enum, value):
        """set(int enum, value) -> None
        tries to set <enum> (e.g. AL_GAIN) for this Listener.
        you can also use the set_ methods (e.g. set_gain),
        which will also update the instance variables (e.g. gain)"""
        if type(value) in (float,):
            alListenerf(ctypes.c_int(enum), ctypes.c_float(value))
        elif type(value) in (ctypes.c_float*3, ctypes.c_float*6):
            alListenerfv(ctypes.c_int(enum), value)
        else:
            try:
                value = tuple(value)
                if len(value) == 3:
                    alListenerfv(ctypes.c_int(enum), (ctypes.c_float*3)(*value))
                elif len(value) == 6:
                    alListenerfv(ctypes.c_int(enum), (ctypes.c_float*6)(*value))
            except: pass

    def move(self, vec3):
        """move(tuple or list vec3) -> None
        moves the Listener by vec3 (dx, dy, dz).
        default position is vec3( 0, 0, 0 )"""
        try:
            self.position = tuple_add3(self.position, vec3)
            self.set(AL_POSITION, self.position)
        except:
            _err("Unsupported argument for move: {}".format(vec3))

    def move_to(self, vec3):
        """move_to(tuple or list vec3) -> None
        moves the Listener to vec3 (x,y,z).
        default is vec3( 0, 0, 0 )"""
        assert len(vec3) == 3, "Argument has to be of length 3"
        try:
            self.position = tuple(vec3)
            self.set(AL_POSITION, self.position)
        except:
            _err("Unsupported argument for move_to: {}".format(vec3))

    def set_position(self,vec3):
        """set_position(tuple or list vec3) -> None
        moves the Listener to vec3 (x,y,z).
        default is vec3( 0, 0, 0 )"""
        assert len(vec3) == 3, "Argument has to be of length 3"
        try:
            self.position = tuple(vec3)
            self.set(AL_POSITION, self.position)
        except:
            _err("Unsupported argument for set_position: {}".format(vec3))

    def set_orientation(self,vec6):
        """set_orientation(tuple or list vec6) -> None
        sets the Listener's orientation to vec6.
        (frontX, frontY, frontZ, upX, upY, upZ)
        default is vec6( 0, 0, -1, 0, 1, 0 )"""
        assert len(vec6) == 6, "Argument has to be of length 6"
        try:
            self.orientation = tuple(vec6)
            self.set(AL_ORIENTATION, self.orientation)
        except:
            _err("Unsupported argument for set_orientation: {}".format(vec6))

    def set_velocity(self, vec3):
        """set_velocity(tuple or list vec3)
        sets the velocity of the Listener to vec3.
        default is vec3( 0, 0, 0 )"""
        assert len(vec3) == 3, "Argument has to be of length 3"
        try:
            self.velocity = tuple(vec3)
            self.set(AL_VELOCITY, self.velocity)
        except:
            _err("Unsupported argument for set_velocity: {}".format(vec3))

    def set_gain(self, value):
        """set_gain(float)
        sets the gain (volume) of the Listener.
        default is 1.0 (100%)"""
        try:
            self.gain = value
            self.set(AL_GAIN, self.gain)
        except:
            _err("Unsupported argument for set_gain: {}".format(value))

_listener = Listener()

def _to_int(i):
    if type(i) in (int, long):
        return i
    elif type(i) in (ctypes.c_int, ctypes.c_char):
        return i.value

def _to_c_int(i):
    if type(i) in (int, long):
        return ctypes.c_int(i)
    elif type(i) in (ctypes.c_uint, ctypes.c_char):
        return ctypes.c_int(i.value)

def _to_c_uint(i):
    if type(i) in (int, long):
        return ctypes.c_uint(i)
    elif type(i) in (ctypes.c_int, ctypes.c_char):
        return ctypes.c_uint(i.value)

def _channels_to_al(ch):
    if ch == 1:
        return(ctypes.c_int(AL_FORMAT_MONO16))
    elif ch == 2:
        return(ctypes.c_int(AL_FORMAT_STEREO16))

class Buffer:
    def __init__(self, *args):
        global _buffers
        _check()
        self._exitsts = True
        self.id = ctypes.c_uint()
        alGenBuffers(1, ctypes.pointer(self.id))

        self.fill(*args)

        _buffers.append(self)

    def _geti(self):
        return ctypes.c_int(self.id.value)

    def _getui(self):
        return self.id

    def destroy(self):
        global _buffers
        if self._exitsts:
            alDeleteBuffers(1, ctypes.pointer(self.id))
            self._exitsts = False
            _buffers.remove(self)

    def fill(self, *args):
        if len(args) == 1:
            file_ = args[0]
            alBufferData(self._getui(), _channels_to_al(file_.channels), file_.buffer, _to_c_int(file_.buffer_length), _to_c_int(file_.frequency))
        else:
            alBufferData(self._getui(), _to_int(args[0]), args[1], _to_int(args[2]), _to_int(args[3]))

class StreamBuffer:
    def __init__(self, stream, count):
        global _buffers
        self.buffer_ids = (ctypes.c_uint * count)()

        alGenBuffers(count, ctypes.cast(ctypes.pointer(self.buffer_ids), ctypes.POINTER(ctypes.c_uint)))

        self.stream = stream

        self.count = 0

        self._exitsts = True

        self.done = False

        for id_ in range(count):
            if self.fill_buffer(id_): self.count += 1

        if self.count < count:
            count_diff = count - self.count
            alDeleteBuffers(count_diff, ctypes.cast(ctypes.pointer((ctypes.c_uint * count_diff)(*self.buffer_ids[-count_diff:])), ctypes.POINTER(ctypes.c_uint)))
            self.buffer_ids = (ctypes.c_uint * self.count)(*self.buffer_ids[:self.count])

        self.last_buffer = self.count - 1

        _buffers.append(self)

    def destroy(self):
        global _buffers
        if self._exitsts:
            alDeleteBuffers(self.count, ctypes.cast(ctypes.pointer(self.buffer_ids), ctypes.POINTER(ctypes.c_uint)))
            self._exitsts = False
            _buffers.remove(self)

    def fill_buffer(self, id_):
        if self._exitsts:
            buffer_info = self.stream.get_buffer()
            if buffer_info:
                buffer_, buffer_size = buffer_info
                alBufferData(_to_c_uint(self.buffer_ids[id_]),  _channels_to_al(self.stream.channels), buffer_, _to_c_int(buffer_size), _to_c_int(self.stream.frequency))
                return True
            else:
                self.done = True
                return False

class Source:
    def __init__(self, buffer_ = None, destroy_buffer = False):
        global _sources
        _check()
        self.id = ctypes.c_uint()
        alGenSources(1, ctypes.pointer(self.id))

        self._exitsts = True

        self.destroy_buffer = destroy_buffer

        self.pitch = 1.

        self.gain = 1.

        self.max_distance = MAX_FLOAT

        self.rolloff_factor = 1.

        self.reference_distance = 1.

        self.min_gain = 0.

        self.max_gain = 1.

        self.cone_outer_gain = 0.

        self.cone_inner_angle = 360.

        self.cone_outer_angle = 360.

        self.position = (0.,0.,0.)

        self.velocity = (0.,0.,0.)

        self.looping = False

        self.direction = (0.,0.,0.)

        self.source_relative = False

        self.source_type = AL_UNDETERMINED

        self._state = AL_INITIAL

        if buffer_:
            self._set_buffer(buffer_)

        _sources.append(self)

    def get(self, enum):
        """get(int enum) -> value
        tries to get <enum> (e.g. AL_GAIN) for this source.
        you can also use the instance variables (e.g. Source.gain)
        (note that instance variables are only updated when calling the
         respective set_ functions (e.g. set_gain))"""
        if enum in (AL_PITCH,
                    AL_GAIN,
                    AL_MIN_GAIN,
                    AL_MAX_GAIN,
                    AL_MAX_DISTANCE,
                    AL_ROLLOFF_FACTOR,
                    AL_CONE_OUTER_GAIN,
                    AL_CONE_INNER_ANGLE,
                    AL_CONE_OUTER_ANGLE,
                    AL_REFERENCE_DISTANCE):
            value = ctypes.c_float()
            alGetSourcef(self.id, enum, value)

        elif enum in (AL_SOURCE_RELATIVE,
                      AL_BUFFER,
                      AL_SOURCE_STATE,
                      AL_BUFFERS_QUEUED,
                      AL_BUFFERS_PROCESSED):
            value = ctypes.c_uint()
            alGetSourcei(self.id, enum, value)

        elif enum in (AL_POSITION,
                      AL_VELOCITY,
                      AL_DIRECTION):
            value = (ctypes.c_float * 3)()
            alGetSourcefv(self.id, enum, value)

        else:
            _err("cannot get({}), this enum doesn't exist or can't be grabbed".format(enum))

        return _to_val(value)

    def set(self, enum, value):
        """set(int enum, value) -> None
        tries to set <enum> (e.g. AL_GAIN) for this source.
        you can also use the set_ methods (e.g. set_gain),
        which will also update the instance variables (e.g. gain)"""
        if type(value) in (float,):
            alSourcef(self.id, ctypes.c_int(enum), ctypes.c_float(value))
        elif type(value) in (int, bool):
            alSourcei(self.id, ctypes.c_int(enum), ctypes.c_int(value))
        elif type(value) in (ctypes.c_float*3, ctypes.c_float*6):
            alSourcefv(self.id,ctypes.c_int(enum), value)
        else:
            try:
                value = tuple(value)
                if len(value) == 3:
                    alSourcefv(self.id, ctypes.c_int(enum), (ctypes.c_float*3)(*value))
                elif len(value) == 6:
                    alSourcefv(self.id, ctypes.c_int(enum), (ctypes.c_float*6)(*value))
            except: pass
            
    def destroy(self):
        global _sources
        """destroy() -> None
        deletes the sources.
        (this is called by oalQuit() automatically)"""
        if self.get_state() == AL_PLAYING:
            self.stop()
        if self.destroy_buffer:
            try:
                self.buffer.destroy()
            except:
                pass
        if self._exitsts:
            alDeleteSources(1, ctypes.pointer(self.id))
            _sources.remove(self)
            self._exitsts = False

    def set_pitch(self, value):
        """set_pitch(float) -> None
        sets the pitch of the source.
        default is 1.0"""
        self.set(AL_PITCH, value)
        self.pitch = _to_val(value)

    def set_gain(self, value):
        """set_gain(float) -> None
        sets the gain (volume) of the source.
        default is 1.0 (100%)"""
        self.set(AL_GAIN, value)
        self.gain = _to_val(value)

    def set_max_distance(self, value):
        """set_max_distance(float) -> None
        sets the maximum attenuation distance of the source.
        (attenuation will not be altered any further after this distance)
        (this does not apply for AL_INVERSE_DISTANCE and AL_EXPONENT_DISTANCE)
        default is MAX_FLOAT"""
        self.set(AL_MAX_DISTANCE, value)
        self.max_distance = _to_val(value)

    def set_rolloff_factor(self, value):
        """set_rolloff_factor(float) -> None
        sets the rolloff_factor of the source.
        default is 1.0"""
        self.set(AL_ROLLOFF_FACTOR, value)
        self.rolloff_factor = _to_val(value)

    def set_reference_distance(self, value):
        """set_reference_distance(float) -> None
        sets the reference_distance of the source.
        default is 1.0"""
        self.set(AL_REFERENCE_DISTANCE, value)
        self.reference_distance = _to_val(value)

    def set_min_gain(self,value):
        """set_min_gain(float) -> None
        sets the minimum gain of the source.
        default is 0.0 (0%)"""
        self.set(AL_MIN_GAIN, value)
        self.min_gain = _to_val(value)

    def set_max_gain(self,value):
        """set_max_gain(float) -> None
        sets the maximum gain of the source.
        default is 1.0 (100%)"""
        self.set(AL_MAX_GAIN, value)
        self.max_gain = _to_val(value)

    def set_cone_outer_gain(self, value):
        self.set(AL_CONE_OUTER_GAIN, value)
        self.cone_outer_gain = _to_val(value)

    def set_cone_inner_angle(self, value):
        self.set(AL_CONE_INNER_ANGLE, value)
        self.cone_inner_angle = _to_val(value)

    def set_cone_outer_angle(self, value):
        self.set(AL_CONE_OUTER_ANGLE, value)
        self.cone_outer_angle = _to_val(value)

    def set_position(self, value):
        """set_position(tuple or list) -> None
        sets the current position of the source.
        default is (0.0, 0.0, 0.0)
        also by default it is non relative,
        if you want to have a relative source,
        please use set_source_relative"""
        self.set(AL_POSITION, value)
        self.position = _to_val(value)

    def set_velocity(self, value):
        """set_velocity(tuple or list) -> None
        sets the current velocity of the source.
        default is (0.0, 0.0, 0.0)"""
        self.set(AL_VELOCITY, value)
        self.velocity = _to_val(value)

    def set_looping(self, value):
        """set_looping(bool) -> None
        wether or not this source should loop playback.
        default is False"""
        self.set(AL_LOOPING, value)
        self.looping = _to_val(value)

    def set_direction(self, value):
        self.set(AL_DIRECTION, value)
        self.direction = _to_val(value)

    def set_source_relative(self, value):
        """set_source_relative(bool) -> None
        wether or not this source should be relative to
        the listener.
        default is False"""
        self.set(AL_SOURCE_RELATIVE, value)
        self.source_relative = _to_val(value)

    def _geti(self):
        return ctypes.c_int(self.id.value)

    def _getui(self):
        return self.id

    def _set_buffer(self, buffer_):
        self.buffer = buffer_

        alSourcei(self.id, AL_BUFFER, self.buffer._geti())

    def get_state(self):
        """get_state() -> int
        returns the current state of the source.
        (e.g. AL_PLAYING, AL_STOPPED, AL_INITIAL)"""
        value = ctypes.c_int()
        alGetSourcei(self.id, AL_SOURCE_STATE, value)
        return value.value

    def play(self):
        """play() -> None
        starts playing the source."""
        alSourcePlay(self.id)
        self._state = AL_PLAYING

    def stop(self):
        """stop() -> None
        stops playing the source."""
        alSourceStop(self.id)
        self._state = AL_STOPPED

    def pause(self):
        """pause() -> None
        pauses playback of the source.
        use play() to continue"""
        alSourcePause(self.id)
        self._state = AL_PAUSED

    def rewind(self):
        """rewind() -> None
        sets playback position to the beginning of the audio track."""
        alSourceRewind(self.id)

    def update(self):
        """update() -> False
        this is a dummy for SourceStream's update() function"""
        return False

class SourceStream(Source):
    def __init__(self, stream):
        global _sources
        _check()
        self.id = ctypes.c_uint()
        alGenSources(1, ctypes.pointer(self.id))

        self._exitsts = True

        self.destroy_buffer = True

        self.pitch = 1.

        self.gain = 1.

        self.max_distance = MAX_FLOAT

        self.rolloff_factor = 1.

        self.reference_distance = 1.

        self.min_gain = 0.

        self.max_gain = 1.

        self.cone_outer_gain = 0.

        self.cone_inner_angle = 360.

        self.cone_outer_angle = 360.

        self.position = (0.,0.,0.)

        self.velocity = (0.,0.,0.)

        self.looping = False

        self.direction = (0.,0.,0.)

        self.source_relative = False

        self.source_type = AL_UNDETERMINED

        self.buffer = StreamBuffer(stream, OAL_STREAM_BUFFER_COUNT)

        self._continue = True

        alSourceQueueBuffers(self.id, self.buffer.count, self.buffer.buffer_ids)

        _sources.append(self)

    def get_state(self):
        """get_state() -> int
        returns the current state of the source.
        (e.g. AL_PLAYING, AL_STOPPED, AL_INITIAL)"""
        value = ctypes.c_int()
        alGetSourcei(self.id, AL_SOURCE_STATE, value)
        
        if value.value == AL_STOPPED and self.buffer.done != True:
            warnings.warn(OalWarning("stream buffer suffocated. Please increase the stream buffer count!"))
            
        return value.value

    def update(self):
        """update() -> bool
        loads some new data into the buffers (if required)
        returns wether or not it is necessary to keep updating"""
        if self._state != AL_PLAYING:
            return
        if self.get_state() == AL_STOPPED:
            self._continue = False
        if self._continue:
            buffers_processed = ctypes.c_int()

            alGetSourcei(self.id, AL_BUFFERS_PROCESSED, ctypes.pointer(buffers_processed))

            for buf_id in range(buffers_processed.value):
                unqueue = self.buffer.last_buffer + 1
                if unqueue >= self.buffer.count:
                    unqueue = 0

                alSourceUnqueueBuffers(self.id, 1, ctypes.pointer(_to_c_uint(self.buffer.buffer_ids[unqueue])))

                buffer_filled = self.buffer.fill_buffer(unqueue)

                if buffer_filled:
                    alSourceQueueBuffers(self.id, 1, ctypes.pointer(_to_c_uint(self.buffer.buffer_ids[unqueue])))

                    self.buffer.last_buffer += 1

                    if self.buffer.last_buffer >= self.buffer.count:
                        self.buffer.last_buffer = 0
                else:
                    self._continue = False

        else:
            buffers_processed = ctypes.c_int()
            alGetSourcei(self.id, AL_BUFFERS_QUEUED, ctypes.pointer(buffers_processed))

            for buf_id in range(buffers_processed.value):
                unqueue = self.buffer.last_buffer + 1
                if unqueue >= self.buffer.count:
                    unqueue = 0
                try:
                    alSourceUnqueueBuffers(self.id, 1, ctypes.pointer(_to_c_uint(self.buffer.buffer_ids[unqueue])))
                except:
                    pass
                self.buffer.last_buffer += 1
                
        return self._continue

def oalGetListener():
    """oalGetListener() -> Listener
    returns the Listener PyOpenAL creates for you."""
    global _listener
    return _listener

def oalQuit():
    """oalQuit() -> None
    destroys all sources and buffers and closes the
    PyOpenAL context and device."""
    global _oaldevice, _oalcontext, _sources, _buffers
    for source in _sources:
        source.destroy()
    for buffer in _buffers:
        buffer.destroy()
    if _oalcontext:
        alcDestroyContext(_oalcontext)
    if _oaldevice:
        alcCloseDevice(_oaldevice)
    _oalcontext = _oaldevice = None
    _sources = []

if PYOGG_AVAIL or WAVE_AVAIL:
    def oalOpen(path, ext_hint=None):
        """oalOpen(filepath [, extension_hint]) -> Source
        loads a wave or ogg file to a source and returns it.
        You can use ext_hint to suggest the file type,
        in case the file extension is not wav, wave, ogg, vorbis or opus"""
        _check()
        if not ext_hint:
            ext_hint = os.path.splitext(path)[1]
        ext_hint = ext_hint.lower()
        if ext_hint in ("ogg", "vorbis", ".ogg", ".vorbis"):
            if not PYOGG_AVAIL:
                _no_pyogg_error()
                return
            file_ = VorbisFile(path)
        elif ext_hint in ("opus", ".opus"):
            if not PYOGG_AVAIL:
                _no_pyogg_error()
                return
            file_ = OpusFile(path)
        elif ext_hint in ("flac", ".flac"):
            if not PYOGG_FLAC_AVAIL:
                _no_pyogg_error()
                return
            file_ = FlacFile(path)
        elif ext_hint in ("wav", ".wav", ".wave", "wave"):
            if not WAVE_AVAIL:
                _err("Wave seems top be unavailable (maybe your Python version doesn't support it...?")
                return
            file_ = WaveFile(path)
        else:
            _err("Unsupported file extension {}. You might want to consider using the ext_hint parameter to pass the file format".format(ext_hint))
            
        buffer_ = Buffer(file_)

        source = Source(buffer_, True)

        return source

    def oalStream(path, ext_hint=None):
        """oalStream(filepath [, extension_hint]) -> SourceStream
        loads a wave or ogg file to a streamed source and returns it.
        You can use ext_hint to suggest the file type,
        in case the file extension is not wav, wave, ogg, vorbis or opus"""
        _check()
        if not ext_hint:
            ext_hint = os.path.splitext(path)[1]
        ext_hint = ext_hint.lower()

        if ext_hint in ("ogg", "vorbis", ".ogg", ".vorbis"):
            if not PYOGG_AVAIL:
                _no_pyogg_error()
                return
            stream = VorbisFileStream(path)
        elif ext_hint in ("opus", ".opus"):
            if not PYOGG_AVAIL:
                _no_pyogg_error()
                return
            stream = OpusFileStream(path)

        elif ext_hint in ("flac", ".flac"):
            if not PYOGG_FLAC_AVAIL:
                _no_pyogg_error()
                return
            stream = FlacFileStream(path)
        elif ext_hint in ("wav", ".wav", "wave", ".wave"):
            if not WAVE_AVAIL:
                _err("Wave seems top be unavailable (maybe your Python version doesn't support it...?")
                return
            stream = WaveFileStream(path)
        else:
            _err("Unsupported file extension {}. You might want to consider using the ext_hint parameter to pass the file format".format(ext_hint))

        return SourceStream(stream)
else:
    oalOpen = _no_pyogg_error
    oalStream = _no_pyogg_error

def _format_enum(enum):
    if len(enum) > 1:
        rest = len(enum)-2

        return ("either {}" + ", {}" * rest + " or {}").format(*enum)
    else:
        return enum

def oalGetALEnum(enum):
    """oalGetALEnum(int enum) -> str
    returns a literal representation of enum"""
    return _format_enum(al_enums.get(enum, []))

def oalGetALCEnum(enum):
    """oalGetALCEnum(int enum) -> str
    returns a literal representation of enum"""
    return _format_enum(alc_enums.get(enum, []))

def oalGetEnum(enum):
    """oalGetEnum(int enum) -> str
    returns a literal representation of enum"""
    al_enum = oalGetALEnum(enum)
    alc_enum = oalGetALCEnum(enum)

    return _format_enum(al_enums.get(enum, []) + alc_enums.get(enum, []))

def oalSetAutoInit(val):
    """oalSetAutoInit(bool) -> None
    wether or not PyOpenAL should initialize automatically
    (default is True)"""
    global OAL_DONT_AUTO_INIT
    OAL_DONT_AUTO_INIT = not val

def oalSetStreamBufferCount(val):
    """oalSetStreamBufferCount(int) -> None
    how many buffers each stream has at a time (at least 2)
    (default is 2)"""
    assert val >= 2, "there have to be at least two StreamBuffers"
    global OAL_STREAM_BUFFER_COUNT
    OAL_STREAM_BUFFER_COUNT = val

def waveSetStreamBufferSize(val):
    """waveSetStreamBufferSize(int) -> None
    how much data each WAVE stream buffer holds (roughly)
    (default is 8192)"""
    global WAVE_STREAM_BUFFER_SIZE
    WAVE_STREAM_BUFFER_SIZE = val
