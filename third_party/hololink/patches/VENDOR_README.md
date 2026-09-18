# Build the Software

1. Unzip the package

```bash
PKG_ZIP=da322_v<X.X.X>_hsb_v<Y.Y.Y>_<hsb_hash>.zip
unzip $PKG_ZIP
```

2. Clone the holoscan-sensor-bridge project

```bash
git clone https://github.com/nvidia-holoscan/holoscan-sensor-bridge.git
cd holoscan-sensor-bridge/
```

3. Check out the commit matching the HSB hash in the release package

```bash
PKG_NAME=${PKG_ZIP%.zip}
HSB_HASH=${PKG_NAME##*_}
git checkout $HSB_HASH
```

4. Apply the modifications

```bash
git apply ../${PKG_NAME}/Host\ Setup\ Scripts/da322_*.patch
```

5. Build the project:

```bash
docker login nvcr.io
sh ./docker/build.sh --igpu
```

# Running applications

## Starting the Hololink container

```bash
  xhost +

  sh ./docker/demo.sh
```

## Running the LI-AR0234CS-STEREO-GMSL2-30 (Leopard Hawk) Mono player application

```bash
	cd examples/
	python linux_ar0234_player.py \
		[--hololink <IP>] \
		[--sensor-id <SENSOR-ID>] \
		[--i2c-id <I2C-ID>] \
		[--log-level <LVL>] \
		[--with-gmsl] \
		[--channel {A,B}] \
		[--help]
```

## Running the LI-AR0234CS-STEREO-GMSL2-30 (Leopard Hawk) Stereo player application

```bash
	cd examples/
	python linux_single_network_stereo_ar0234_player.py \
		[--hololink <IP>] \
		[--sensor-id-left <SENSOR-ID-LEFT>] \
		[--sensor-id-right <SENSOR-ID-RIGHT>] \
		[--i2c-id <I2C-ID>] \
		[--log-level <LVL>] \
		[--with-gmsl] \
		[--channel {A,B}] \
		[--help]
```

## Running the IMX219 player application

```bash
	cd examples/
	python linux_imx219_player.py \
		[--hololink <IP>] \
		[--sensor-id <ID>] \
		[--camera-mode {0,1,2,3}] \
		[--log-level <LVL>] \
		[--help]
```

## Running the Arducam B0249 player application

```bash
	cd examples/
	python linux_imx477_player.py \
		[--hololink <IP>] \
		[--sensor-id <ID>] \
		[--camera-mode {0,1,2,3}] \
		[--log-level <LVL>] \
		[--help]
```

## Running the Arducam B0353 player application

```bash
	cd examples/
	python linux_arducam_b0353_player.py \
		[--hololink <IP>] \
		[--sensor-id <ID>] \
		[--camera-mode {0,1,2,3}] \
		[--log-level <LVL>] \
		[--help]
```

## Running the X1300 HDMI to CSI-2 bridge player application

```bash
	cd examples/
	python linux_tc358743_player.py \
		[--hololink <IP>] \
		[--sensor-id <ID>] \
		[--width <WIDTH>] \
		[--height <HEIGHT>] \
		[--log-level <LVL>] \
		[--help]
```

## Running the LI-AR0234CS-STEREO-GMSL2-30 (Leopard Hawk) FuSa CoE player application

```bash
	cd examples/
	python fusa_coe_ar0234_player.py \
		[--hololink <IP>] \
		[--sensor {-1,0,1}] \
		[--i2c-id <I2C-ID>] \
		[--log-level <LVL>] \
		[--with-gmsl] \
		[--channel {A,B}] \
        [--interface <IFC>] \
		[--help]
```

*NOTE*: For FuSa CoE applications, the connection needs to be clean.
    To make sure of that, power cycle or reboot the Host and HSB boards before running the application.
