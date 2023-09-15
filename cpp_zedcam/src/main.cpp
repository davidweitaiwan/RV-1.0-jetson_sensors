#include <map>

#include <sl/Camera.hpp>

#include "header.h"

cv::Mat slMat2cvMat(sl::Mat& input)
{
	// Mapping between MAT_TYPE and CV_TYPE
	int cv_type = -1;
	switch (input.getDataType()) {
	case sl::MAT_TYPE::F32_C1: cv_type = CV_32FC1; break;
	case sl::MAT_TYPE::F32_C2: cv_type = CV_32FC2; break;
	case sl::MAT_TYPE::F32_C3: cv_type = CV_32FC3; break;
	case sl::MAT_TYPE::F32_C4: cv_type = CV_32FC4; break;
	case sl::MAT_TYPE::U8_C1: cv_type = CV_8UC1; break;
	case sl::MAT_TYPE::U8_C2: cv_type = CV_8UC2; break;
	case sl::MAT_TYPE::U8_C3: cv_type = CV_8UC3; break;
	case sl::MAT_TYPE::U8_C4: cv_type = CV_8UC4; break;
	default: break;
	}

	// Since cv::Mat data requires a uchar* pointer, we get the uchar1 pointer from sl::Mat (getPtr<T>())
	// cv::Mat and sl::Mat will share a single memory structure
	return cv::Mat(input.getHeight(), input.getWidth(), cv_type, input.getPtr<sl::uchar1>(sl::MEM::CPU));
}

void RunZEDProc(sl::Camera& zed, std::shared_ptr<ZEDNode> node, int topicID, bool& stopF)
{
    // Set runtime parameters after opening the camera
    auto params = node->getParams();
    sl::RuntimeParameters runtime_parameters;
    if (params->camera_sensing_mode == 0)// Standard mode
        // runtime_parameters.sensing_mode = sl::SENSING_MODE::STANDARD;// SDK ver 3.8
        runtime_parameters.enable_fill_mode = false;
    else if (params->camera_sensing_mode == 1)// Fill mode
        // runtime_parameters.sensing_mode = sl::SENSING_MODE::FILL;// SDK ver 3.8
        runtime_parameters.enable_fill_mode = true;
    
    sl::Mat rgbslMat, depthslMat;
    cv::Mat rgbMat, depthMat;

    // TODO: add ZED grab process and ROS2 publish process
    sl::Resolution pubRGBImgSize(params->topic_ZEDCam_RGB_width, params->topic_ZEDCam_RGB_height);
    sl::Resolution pubDepthImgSize(params->topic_ZEDCam_Depth_width, params->topic_ZEDCam_Depth_height);

    std::vector<int> encodeParam;
    encodeParam.push_back(cv::IMWRITE_JPEG_QUALITY);
    encodeParam.push_back(70);
    std::vector<uchar> pubRGBImgVec;
    std::vector<uchar> pubDepthImgVec;

    // Check ZED opened
    while (!zed.isOpened() && !stopF)
        std::this_thread::sleep_for(1s);
    
    if (stopF)
        return;

    // Grab image to check resolution
    if (zed.grab(runtime_parameters) == sl::ERROR_CODE::SUCCESS)
    {
        if (params->camera_use_color)
        {
            zed.retrieveImage(rgbslMat, sl::VIEW::LEFT, sl::MEM::CPU, pubRGBImgSize);
            if (rgbslMat.getResolution() != pubRGBImgSize)
                printf("The RGB image will be resized into %dx%d. Current size: %ldx%ld\n", 
                    pubRGBImgSize.width, pubRGBImgSize.height, rgbslMat.getWidth(), rgbslMat.getHeight());
        }
        if (params->camera_use_depth)
        {
            zed.retrieveMeasure(depthslMat, sl::MEASURE::DEPTH, sl::MEM::CPU, pubDepthImgSize);
            if (depthslMat.getResolution() != pubDepthImgSize)
                printf("The Depth image will be resized into %dx%d. Current size: %ldx%ld\n", 
                    pubDepthImgSize.width, pubDepthImgSize.height, depthslMat.getWidth(), depthslMat.getHeight());
        }
    }
    else
    {
        std::cerr << "Unable to retrieve image\n";
        zed.close();
        return;
    }

    // Loop to grab image
    while (!stopF)
    {
CHECK_OPEN:
        // Check ZED opened
        while (!zed.isOpened())
            continue;

        // Grab image from ZED camera
        if (zed.grab(runtime_parameters) == sl::ERROR_CODE::SUCCESS)
        {
            if (params->camera_use_color)
            {
                zed.retrieveImage(rgbslMat, sl::VIEW::LEFT, sl::MEM::CPU, pubRGBImgSize);
                rgbMat = slMat2cvMat(rgbslMat);
            }
            if (params->camera_use_depth)
            {
                zed.retrieveMeasure(depthslMat, sl::MEASURE::DEPTH, sl::MEM::CPU, pubDepthImgSize);
                //depthMat = slMat2cvMat(depthslMat);
            }

            // Process image size and publish to ROS2 topic
            auto& pub = node->getZEDPublisher(topicID);
            if (params->camera_use_color)
            {
                cv::imencode(".jpg", rgbMat, pubRGBImgVec, encodeParam);
                pub->pubRGBMat(pubRGBImgVec, rgbMat.size(), rgbMat.type());
            }
            if (params->camera_use_depth)
            {
                uint32_t step = depthslMat.getStepBytes();
                size_t size = step * depthslMat.getHeight();
                uint8_t * data_ptr = nullptr;
                if (depthslMat.getDataType() == sl::MAT_TYPE::F32_C1)
                {
                    data_ptr = reinterpret_cast<uint8_t *>(depthslMat.getPtr<sl::float1>());
                    pubDepthImgVec = std::vector<uint8_t>(data_ptr, data_ptr + size);
                    pub->pubDepthMat(pubDepthImgVec, cv::Size(depthslMat.getWidth(), depthslMat.getHeight()), CV_32FC1, params->camera_depth_unit);
                }
            }
        }
        else
        {
            std::cerr << "Unable to retrieve image\n";
            goto CHECK_OPEN;
        }
    }
}

void ScanZEDDevice(std::shared_ptr<ZEDNode> zedNode, sl::InitParameters init_parameters, std::vector<double> zedDevVec, bool useIDF, std::vector<double> ids, bool& stopF)
{
    std::map<int, int> zedIDs;// {zedSN, topicID}
    std::map<int, sl::Camera> zeds;// {zedSN, sl::Camera}
    std::vector<std::thread> zedThVec;

    while (!stopF)
    {
        // Search available ZED camera
        std::map<int, int> _zedIDs;
        std::vector<sl::DeviceProperties> zedDevList = sl::Camera::getDeviceList();
        for (auto& i : zedDevList)
        {
            std::cout << i.camera_model << " " << i.id << " [" << i.serial_number << "]" << " [" << i.camera_state << "]" << "\n";
            // std::cout << i.path << "\n";
            int chkID = useIDF ? i.id + (i.camera_model == sl::MODEL::ZED_X ? 10 : 0) : i.serial_number;

            for (int j = 0; j < zedDevVec.size(); j++)
            {
                if (chkID == static_cast<int>(zedDevVec[j]))
                {
                    if (zedIDs.find(i.serial_number) == zedIDs.end())
                    {
                        zedIDs[i.serial_number] = static_cast<int>(ids[j]);
                        _zedIDs[i.serial_number] = static_cast<int>(ids[j]);
                    }
                    break;
                }
            }
        }

        // Open ZED cameras
        for (auto& [zedID, topicID] : _zedIDs)
        {
            auto zParams = init_parameters;
            zParams.input.setFromSerialNumber(zedID);
            zeds[zedID] = sl::Camera();
            sl::ERROR_CODE returned_state = zeds[zedID].open(zParams);
            if (returned_state == sl::ERROR_CODE::SUCCESS)
            {
                zedNode->addZEDPublisher(topicID);
                zedThVec.emplace_back(RunZEDProc, std::ref(zeds[zedID]), zedNode, topicID, std::ref(stopF));
                std::cout << "ZED " << zedID << " will be related to topic " << topicID << "\n";
            }
            else
            {
                std::cout << "ZED " << zedID << " Error " << returned_state << ", ignore.\n";
                zeds.erase(zedID);
            }
        }
        std::this_thread::sleep_for(5s);
    }

    for (auto& i : zedThVec)
        i.join();
    
    for (auto& [id, zed] : zeds)
        zed.close();
}

int main(int argc, char** argv)
{
    rclcpp::init(argc, argv);

    auto params = std::make_shared<Params>("zed_params_node");
    if (params->camera_cap_input_type != "id" && params->camera_cap_input_type != "sn")
    {
        RCLCPP_ERROR(params->get_logger(), "[main] cap input type error.");
        return EXIT_FAILURE;
    }

    bool useIDF = params->camera_cap_input_type == "id";
    std::vector<double> zedDevVec = useIDF ? params->camera_cap_ids : params->camera_cap_sns;
    if (params->ids.size() != zedDevVec.size())
    {
        RCLCPP_ERROR(params->get_logger(), "[main] ids size not fit zed device size.");
        return EXIT_FAILURE;
    }

    /**
     * ZED camera settings
     * ref: https://github.com/stereolabs/zed-examples/blob/master/tutorials/tutorial%203%20-%20depth%20sensing/cpp/main.cpp
    */
    sl::InitParameters init_parameters;
    init_parameters.camera_resolution = sl::RESOLUTION::HD720;
    if (params->camera_height >= 1080)
        init_parameters.camera_resolution = sl::RESOLUTION::HD1080;
    else if (params->camera_height >= 720)
        init_parameters.camera_resolution = sl::RESOLUTION::HD720;
    else if (params->camera_height >= 360)
        init_parameters.camera_resolution = sl::RESOLUTION::VGA;

    if (params->camera_depth_quality == 0)// Performance mode
        init_parameters.depth_mode = sl::DEPTH_MODE::PERFORMANCE;
    else if (params->camera_depth_quality == 1)// Quality mode
        init_parameters.depth_mode = sl::DEPTH_MODE::QUALITY;
    
    if (params->camera_depth_unit == 1)// Unit: millimeter
        init_parameters.coordinate_units = sl::UNIT::MILLIMETER;
    else if (params->camera_depth_unit == 2)// Unit: centimeter
        init_parameters.coordinate_units = sl::UNIT::CENTIMETER;
    else if (params->camera_depth_unit == 3)// Unit: meter
        init_parameters.coordinate_units = sl::UNIT::METER;
    
    init_parameters.camera_fps = params->camera_fps;

    bool stopF = false;
    auto zedNode = std::make_shared<ZEDNode>(params);

    // Scan ZED device
    std::thread scanTh = std::thread(ScanZEDDevice, zedNode, init_parameters, zedDevVec, useIDF, params->ids, std::ref(stopF));

    // Start zedNode spin
    std::thread zedNodeTh = std::thread(vehicle_interfaces::SpinNode, zedNode, "zedNodeTh");

    scanTh.join();
    zedNodeTh.join();

    rclcpp::shutdown();
    return EXIT_SUCCESS;
}