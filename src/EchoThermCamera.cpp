#include "EchoThermCamera.h"
#include <iostream>
#include <fcntl.h>
#include <cstring>

#ifdef __linux__
#include <linux/videodev2.h>
#include <unistd.h>
#include <sys/ioctl.h>
#endif

EchoThermCamera::EchoThermCamera(std::string const& devicePath, seekcamera_frame_format_t format, seekcamera_color_palette_t colorPalette, seekcamera_shutter_mode_t shutterMode)
	: m_devicePath{devicePath}
	, m_format{format}
	, m_colorPalette{ colorPalette }
	, m_shutterMode{ shutterMode }
	, mp_camera{nullptr}
	, m_mut{}
	, m_device{ -1 }
	, m_timeoutCount{0}
{
}

EchoThermCamera::~EchoThermCamera()
{
	disconnect();
}


EchoThermCamera::EchoThermCamera(EchoThermCamera&& that)
    : m_devicePath{std::move(that.m_devicePath)}
    , m_format{std::move(that.m_format)}
    , m_colorPalette{std::move(that.m_colorPalette)}
	, m_shutterMode{std::move(that.m_shutterMode)}
    , mp_camera{that.mp_camera}
    , m_mut{}
    , m_device{that.m_device}
{
    that.mp_camera=nullptr;
    that.m_device=-1;
}


std::string const& EchoThermCamera::getDevicePath() const
{
	return m_devicePath;
}
seekcamera_frame_format_t EchoThermCamera::getFormat() const
{
	return m_format;
}
seekcamera_color_palette_t EchoThermCamera::getColorPalette() const
{
	return m_colorPalette;
}

seekcamera_shutter_mode_t EchoThermCamera::getShutterMode() const
{
	return m_shutterMode;
}

seekcamera_error_t EchoThermCamera::openSession(bool reconnect)
{
	// Register a frame available callback function.
	seekcamera_error_t status = SEEKCAMERA_SUCCESS;
	if(!reconnect)
	{
		status = seekcamera_register_frame_available_callback(mp_camera, [](seekcamera_t*, seekcamera_frame_t* p_cameraFrame, void* p_userData) {
		auto* p_cameraData = (EchoThermCamera*)p_userData;
		p_cameraData->handleCameraFrameAvailable(p_cameraFrame);
		}, (void*)this);
	}
	if (status == SEEKCAMERA_SUCCESS)
	{
		// set pipeline mode to SeekVision
		status = seekcamera_set_pipeline_mode(mp_camera, SEEKCAMERA_IMAGE_SEEKVISION);
		if (status == SEEKCAMERA_SUCCESS)
		{
			// Start the capture session.
			status = seekcamera_capture_session_start(mp_camera, m_format);
			if (status == SEEKCAMERA_SUCCESS)
			{
				auto const shutterStatus = seekcamera_set_shutter_mode(mp_camera, m_shutterMode);
				if(shutterStatus != SEEKCAMERA_SUCCESS)
				{
					::std::cout << "failed to set shutter mode to " << (m_shutterMode == SEEKCAMERA_SHUTTER_MODE_AUTO ? "auto" : "manual") << ": " << seekcamera_error_get_str(shutterStatus) << std::endl;
					status = shutterStatus;
				}
				auto const paletteStatus = seekcamera_set_color_palette(mp_camera, m_colorPalette);
				if(paletteStatus != SEEKCAMERA_SUCCESS)
				{
					::std::cout << "failed to set color palette to " << seekcamera_color_palette_get_str(m_colorPalette) << ": " << seekcamera_error_get_str(paletteStatus) << std::endl;
					status = paletteStatus;
				}
			}
			else
			{
				std::cerr << "failed to start capture session: " << seekcamera_error_get_str(status) << std::endl;
			}
		}
		else
		{
			std::cerr << "failed to set image pipeline mode: " << seekcamera_error_get_str(status) << std::endl;
		}
	}
	else
	{
		std::cerr << "failed to register frame callback: " << seekcamera_error_get_str(status) << std::endl;
	}
	return status;
}

seekcamera_error_t EchoThermCamera::connect(seekcamera_t* p_camera)
{
	disconnect();
	mp_camera = p_camera;
	return openSession(false);
}


seekcamera_error_t EchoThermCamera::closeSession()
{
	auto const status = seekcamera_capture_session_stop(mp_camera);
	if (status != SEEKCAMERA_SUCCESS)
	{
		std::cerr << "failed to stop capture session: " << seekcamera_error_get_str(status) << std::endl;
	}
	return status;
}


seekcamera_error_t EchoThermCamera::disconnect()
{
	m_timeoutCount=0;
	seekcamera_error_t status = SEEKCAMERA_SUCCESS;
	if (mp_camera)
	{
		status = closeSession();
		mp_camera = nullptr;
	}
	if (m_device >= 0)
	{
		close(m_device);
		m_device = -1;
	}
	return status;
}

seekcamera_error_t EchoThermCamera::handleReadyToPair(seekcamera_t* p_camera)
{
	// Attempt to pair the camera automatically.
	// Pairing refers to the process by which the sensor is associated with the host and the embedded processor.
	seekcamera_error_t status = seekcamera_store_calibration_data(p_camera, nullptr, nullptr, nullptr);
	if (status != SEEKCAMERA_SUCCESS)
	{
		std::cerr << "failed to pair device: " << seekcamera_error_get_str(status) << std::endl;
	}
	// Start imaging.
	status = connect(p_camera);
	return status;
}

int EchoThermCamera::openDevice(int width, int height)
{
	//TODO find a way to detect the format automatically
#ifdef __linux__
	int returnCode = 0;
	m_device = open(m_devicePath.c_str(), O_RDWR);
	if (m_device < 0)
	{
		returnCode = m_device;
		std::cerr << "Error opening v4l2 device: " << strerror(errno) << " on device with path '" << m_devicePath << "'" << std::endl;
	}
	else
	{
		struct v4l2_format v;
		v.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
		returnCode = ioctl(m_device, VIDIOC_G_FMT, &v);
		if (returnCode < 0)
		{
			std::cerr << "VIDIOC_G_FMT error: " << strerror(errno) << " on device with path '" << m_devicePath << "'" << std::endl;
		}
		else
		{
			v.fmt.pix.width = width;
			v.fmt.pix.height = height;
			switch (m_format)
			{
			case SEEKCAMERA_FRAME_FORMAT_COLOR_YUY2:
				v.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
				v.fmt.pix.sizeimage = width * height * 2;
				break;
			default:
				std::cerr << "Unknown format " << m_format << ::std::endl;
				returnCode = -1;
				break;
			}

			if(returnCode>=0)
			{
				returnCode = ioctl(m_device, VIDIOC_S_FMT, &v);
				if (returnCode < 0)
				{
					std::cerr << "VIDIOC_S_FMT error: " << strerror(errno) << " on device with path '" << m_devicePath << "'" << std::endl;
				}
				else
				{
					std::cout << "Opened v4l2 device with path '" << m_devicePath << "'" << std::endl;
				}
			}
		}
	}

#else
	int returnCode = = -1;
	std::cout << "v4l2 is not supported on this platform" << std::endl;
#endif
	return returnCode;
}

int EchoThermCamera::recordTimeout()
{
	return ++m_timeoutCount;
}

void EchoThermCamera::resetTimeouts()
{
	m_timeoutCount=0;
}

void EchoThermCamera::handleCameraFrameAvailable(seekcamera_frame_t* p_cameraframe)
{
	// This mutex is used to serialize access to the frame member of the renderer.
	// In the future the single frame member should be replaced with something like a FIFO
	std::lock_guard<std::mutex> guard(m_mut);
	// Lock the seekcamera frame for safe use outside of this callback.
	seekcamera_frame_lock(p_cameraframe);
	// Get the frame to draw.
	seekframe_t* p_frame = nullptr;
	seekcamera_error_t status = seekcamera_frame_get_frame_by_format(p_cameraframe, m_format, &p_frame);
	if (status == SEEKCAMERA_SUCCESS)
	{
		if (m_device < 0)
		{
			int const frameWidth = (int)seekframe_get_width(p_frame);
			int const frameHeight = (int)seekframe_get_height(p_frame);
			openDevice(frameWidth, frameHeight);
		}
		if (m_device >= 0)
		{			
			void* const p_frameData = seekframe_get_data(p_frame);
			size_t const frameDataSize = seekframe_get_data_size(p_frame);
			ssize_t written = write(m_device, p_frameData, frameDataSize);
			if (written < 0)
			{
				std::cerr << "Error writing "<<frameDataSize<<" bytes to v4l2 device : " << strerror(errno) << " on device with path '" << m_devicePath << "'" << std::endl;
			}
		}
	}
	else
	{
		std::cerr << "failed to get frame: " << seekcamera_error_get_str(status) << std::endl;
	}
	seekcamera_frame_unlock(p_cameraframe);
	resetTimeouts();

}


seekcamera_error_t EchoThermCamera::triggerShutter()
{
	auto const status = seekcamera_shutter_trigger(mp_camera);
	if(status != SEEKCAMERA_SUCCESS)
	{
		std::cerr << "failed to trigger shutter: " << seekcamera_error_get_str(status) << std::endl;
	}
	return status;
}

