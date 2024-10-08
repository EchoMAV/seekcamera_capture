#include "EchoThermCameraLoopHandler.h"
#include "EchoThermCamera.h"
#include <iostream>
#include <filesystem>
#include <fstream>
#include <boost/property_tree/json_parser.hpp>
#include <boost/program_options.hpp>
#include <sys/inotify.h>
#include <csignal>

EchoThermCameraLoopHandler g_loopHandler;

void sighandler(int signum)
{
	std::cout << std::endl;
	std::cout << "Caught termination signal";
	std::cout << std::endl;
	g_loopHandler.stop();
	exit(0);
}

void writeConfig(::std::filesystem::path const& path, ::std::unordered_map<::std::string, EchoThermCamera> & cameraMap)
{
	boost::property_tree::ptree properties;
	boost::property_tree::ptree cameraArray;
	properties.put("default_color_palette", (int)SEEKCAMERA_COLOR_PALETTE_SPECTRA);
	properties.put("default_shutter_mode", (int)SEEKCAMERA_SHUTTER_MODE_AUTO);
	for(auto const& [cid, camera] : cameraMap)
	{
		boost::property_tree::ptree cameraProps;
		cameraProps.put("cid", cid);
		cameraProps.put("device_path", camera.getDevicePath());
		cameraProps.put("format", camera.getFormat());
		cameraArray.push_back(::std::make_pair("", cameraProps));
	}
	properties.add_child("camera_array", cameraArray);
	::std::ofstream outputStream(path);
	boost::property_tree::write_json(outputStream, properties, true);
}

int readConfig(std::filesystem::path const& configFilePath, ::std::unordered_map<::std::string, EchoThermCamera> & cameraMap)
{
	int returnCode=1;
	if(std::filesystem::exists(configFilePath)
		&& std::filesystem::is_regular_file(configFilePath))
	{
		std::cout<<"configFile exists"<<std::endl;
		boost::property_tree::ptree properties;
		try
		{
			::std::ifstream inputStream(configFilePath);
			boost::property_tree::read_json(inputStream,properties);
			seekcamera_color_palette_t defaultColorPallete = (seekcamera_color_palette_t)properties.get<int>("default_color_palette");
			seekcamera_shutter_mode_t defaultShutterMode = (seekcamera_shutter_mode_t)properties.get<int>("default_shutter_mode");
			seekcamera_frame_format_t defaultFrameFormat = (seekcamera_frame_format_t)properties.get<int>("default_frame_format");
			g_loopHandler.setDefaultColorPalette(defaultColorPallete);
			g_loopHandler.setDefaultShutterMode(defaultShutterMode);
			g_loopHandler.setDefaultFrameFormat(defaultFrameFormat);
			auto cameraArrayOpt = properties.get_child_optional("camera_array");
			if(cameraArrayOpt.is_initialized())
			{
				for(auto const& [key,cameraProps] : cameraArrayOpt.get())
				{
					auto cid = cameraProps.get<std::string>("cid");
					auto devicePath = cameraProps.get<std::string>("device_path");
					auto format = (seekcamera_frame_format_t)cameraProps.get<int>("format");
					auto colorPalette = (seekcamera_color_palette_t)cameraProps.get<int>("color_palette", (int)defaultColorPallete);
					auto shutterMode = (seekcamera_shutter_mode_t)cameraProps.get<int>("shutter_mode", (int)defaultShutterMode);

					EchoThermCamera camera(devicePath, format, colorPalette, shutterMode);
					cameraMap.emplace(cid, ::std::move(camera));
				}
			}
			returnCode=0;
		}
		catch(...)
		{
			std::cerr<<"exception while reading configFile "<<configFilePath.string()<<std::endl;
		}
	}
	return returnCode;
}

int main(int argc, char* argv[])
{
	signal(SIGINT, sighandler);
	signal(SIGTERM, sighandler);
	boost::program_options::options_description desc("Allowed options");
	desc.add_options()
		("help", "produce help message")
		("configFile", boost::program_options::value<std::filesystem::path>(), "choose the config file path");
	boost::program_options::variables_map vm;
	boost::program_options::store(boost::program_options::parse_command_line(argc, argv, desc), vm);
	boost::program_options::notify(vm);
	std::filesystem::path configFilePath{};
	std::unordered_map<::std::string, EchoThermCamera> cameraMap{};
	bool proceed=true;
	if(vm.count("help"))
	{
		std::cout<<desc<<::std::endl;
		proceed=false;
	}
	else
	{
		if(vm.count("configFile"))
		{
			configFilePath = vm["configFile"].as<std::filesystem::path>();
			std::cout<<"configFile = "<<configFilePath<<std::endl;
			//EchoThermCamera echoThermCamera("/dev/video0", SEEKCAMERA_FRAME_FORMAT_COLOR_YUY2, SEEKCAMERA_COLOR_PALETTE_WHITE_HOT);
			//cameraMap.emplace("E452AFB41114", ::std::move(echoThermCamera)).first;
			//std::cout<<"writing to config file "<<std::filesystem::absolute(configFilePath)<<std::endl;
			//writeConfig(configFilePath, cameraMap);
		}
		else
		{
			std::cout<<"no options found"<<std::endl;
		}
	}
	int notifyFileDescriptor = inotify_init();
	int notifyWatchDescriptor = -1;
	if(!configFilePath.empty())
	{
		if(notifyFileDescriptor < 0)
		{
			std::cerr<<"inotify_init failed: "<<strerror(errno)<<std::endl;
		}
		else
		{
			notifyWatchDescriptor = inotify_add_watch(notifyFileDescriptor, configFilePath.c_str(),  IN_MODIFY | IN_CREATE | IN_DELETE);
			if(notifyWatchDescriptor < 0)
			{
				std::cerr<<"inotify_add_watch on file '"<<configFilePath<<"' failed: "<<strerror(errno)<<std::endl;
			}
		}
	}
	int returnCode=0;
	if(notifyFileDescriptor >=0 && notifyWatchDescriptor>=0)
	{
		std::cout<<"watching config file"<<std::endl;
		//watch a config file and reload when it changes
		for(;;)
		{
			if(readConfig(configFilePath, cameraMap))
			{
				cameraMap.clear();
			}
			auto status = g_loopHandler.start(::std::move(cameraMap));
			if (status != SEEKCAMERA_SUCCESS)
			{
				break;
			}
			std::cout<<"started camera manager"<<std::endl;
			char p_buffer[sizeof(inotify_event) + 16];
			// wait until the configuration file is modified
			auto const length=read(notifyFileDescriptor, p_buffer, sizeof(inotify_event) + 16);
			std::cout<<"change in config file detected"<<std::endl;
			if(length<0)
			{
				std::cerr<<"read failed: "<<strerror(errno)<<std::endl;
				break;
			}
			g_loopHandler.stop();
			cameraMap.clear();
		}
	}
	else
	{
		returnCode=1;
		std::cerr<<"A configFile is required"<<std::endl;
	}
	(void)inotify_rm_watch( notifyFileDescriptor, notifyWatchDescriptor );
	(void)close( notifyFileDescriptor );
	return 0;
}
