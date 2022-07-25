sudo ln -s /home/x/Documents/deepstream-6.1/sources/gst-plugins/gst-dsrclcpp/libnvds_demo.so  /usr/lib/aarch64-linux-gnu/gstreamer-1.0/deepstream/libnvds_demo.so


gst-launch-1.0 filesrc location=/home/x/Documents/deepstream-6.1/samples/streams/sample_720p.h264  ! h264parse ! nvv4l2decoder ! m.sink_0 nvstreammux name=m batch-size=1 width=1280 height=720 ! nvinfer config-file-path=/home/x/Documents/deepstream-6.1/sources/objectDetector_Yolo/config_infer_primary_yoloV5.txt ! nvvideoconvert ! dsdemo  full-frame=0  ! nvdsosd ! nvegltransform ! nveglglessink
