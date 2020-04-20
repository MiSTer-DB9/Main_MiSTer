FROM theypsilon/gcc-linaro:6.5.0
WORKDIR /project
ADD . /project
RUN /opt/intelFPGA_lite/quartus/bin/quartus_sh --flow compile make
CMD cat /project/MiSTer
