import sensor, image, time, ure, uos, lcd, gc, ubinascii, pmu
from Maix import GPIO
from fpioa_manager import fm, board_info
from machine import UART

class App():
    def main(self):
        self.setup()
        while(True):
            self.loop()

    def setup(self):
        fm.register(35, fm.fpioa.UART2_TX, force=True)
        fm.register(34, fm.fpioa.UART2_RX, force=True)
        baud = 1500000 # 115200 1500000 3000000 4500000
        self.uart = UART(UART.UART2, baud, 8, 0, 0, timeout=1000, read_buf_len=4096)

        fm.register(board_info.LED_W, fm.fpioa.GPIO3)
        fm.register(board_info.LED_R, fm.fpioa.GPIO4)
        fm.register(board_info.LED_G, fm.fpioa.GPIO5)
        fm.register(board_info.LED_B, fm.fpioa.GPIO6)
        self.gpio_led_w = GPIO(GPIO.GPIO3, GPIO.OUT)
        self.gpio_led_r = GPIO(GPIO.GPIO4, GPIO.OUT)
        self.gpio_led_g = GPIO(GPIO.GPIO5, GPIO.OUT)
        self.gpio_led_b = GPIO(GPIO.GPIO6, GPIO.OUT)
        self.gpio_led_w.value(1)
        self.gpio_led_r.value(1)
        self.gpio_led_g.value(1)
        self.gpio_led_b.value(1)

        self.img_seq = 0
        self.counter = 0
        self.light_on_ms = None
        self._devicename = self.devicename()
        self.sensor_reset("RGB565", "QVGA")
        self.update_display(init=True)

    def loop(self):
        gc.collect()
        self.light_ctrl(False)

        self.counter += 1
        line = self.readLineFromC()

        if line == None:
            if self.counter % 1000 == 0:
                line = ""
            else:
                time.sleep(0.001)
                return
        #print("counter=%d ms=%d line=[%s]" % (self.counter, time.ticks_ms(), line))

        m = ure.search("cmd=RESET-REQ pixformat=(\S+) framesize=(\S+)", line)
        if m:
            pixformat = m.group(1)
            framesize = m.group(2)

            self.sensor_reset(pixformat, framesize)
            self.sendStringToC("cmd=RESET-RESP result=OK\n")

        m = ure.search("cmd=SNAPSHOT-REQ format=(\S+) quality=(\S+)", line)
        if m:
            format = m.group(1)
            quality = int(m.group(2))
            if format=="JPEG":
                self.img_seq += 1
                self.light_ctrl(True)
                self.img = sensor.snapshot()
                if False:
                    self.img.draw_rectangle(0, 0, 150, 50, color=(128,128,128), fill=True)
                    self.img.draw_string(2, 2, "%d" %(self.img_seq), color=(255,255,255), scale=5, mono_space=False)
                self.img.compress(quality)
                self.img_bytes = self.img.to_bytes()
                self.img_size = len(self.img_bytes)
                self.sendStringToC("cmd=SNAPSHOT-RESP seq=%d size=%d\n" % (self.img_seq, self.img_size))

        m = ure.search("cmd=DATA-REQ seq=(\d+) offset=(\d+) length=(\d+)", line)
        if m:
            seq    = int(m.group(1))
            offset = int(m.group(2))
            length = int(m.group(3))
            if seq==self.img_seq and 0<=offset and offset+length<=self.img_size:
                bytes = self.img_bytes[offset:(offset+length)]
                crc32 = ubinascii.crc32(bytes)
                self.sendStringToC("cmd=DATA-RESP seq=%d offset=%d length=%d crc32=%d\n" % (self.img_seq, offset, length, crc32))
                self.sendDataToC(bytes)

        m = ure.search("cmd=PING msg=(\S+)", line)
        if m:
            msg = m.group(1)
            self.sendStringToC("cmd=PONG msg=%s\n" % (msg))

    def sensor_reset(self, pixformat, framesize):
        sensor.reset()
        if pixformat=="RGB565":
            sensor.set_pixformat(sensor.RGB565)
        if pixformat=="GRAYSCALE":
            sensor.set_pixformat(sensor.GRAYSCALE)

        if framesize=="VGA":
            sensor.set_framesize(sensor.VGA)
        if framesize=="QVGA":
            sensor.set_framesize(sensor.QVGA)
        if framesize=="QQVGA":
            sensor.set_framesize(sensor.QQVGA)
        sensor.skip_frames()

    def light_ctrl(self, on):
        if on:
            if not self.light_on_ms:
                # ON
                if self._devicename=="M5StickV":
                    self.gpio_led_w.value(0)
                elif self._devicename=="UnitV":
                    pass
            self.light_on_ms = time.ticks_ms()
        else:
            if self.light_on_ms and self.light_on_ms + 2000 <= time.ticks_ms():
                # OFF
                if self._devicename=="M5StickV":
                    self.gpio_led_w.value(1)
                elif self._devicename=="UnitV":
                    pass
                self.light_on_ms = None



    def sendDataToC(self, data):
        #print("sendDataToC: len=%d" % (len(data)))
        l = self.uart.write(data)
        if l!=len(data):
            print("sendDataToC: ERROR!!!")

    def sendStringToC(self, s):
        #print("sendStringToC: " + s)
        self.uart.write(s)

    def readFromC(self, num):
        return self.uart.read(num)

    def readLineFromC(self):
        line = self.uart.readline()
        if line and line[0] != 0:
            try:
                line = line.decode('ascii').strip()
            except UnicodeError as e:
                line = None
        else:
            line = None
        return line

    def devicename(self):
        from machine import I2C
        i2c = I2C(I2C.I2C0, freq=100000, scl=28, sda=29)
        devices = i2c.scan()
        if len(devices)==0:
            return "UnitV"
        else:
            return "M5StickV"

    def update_display(self, init=False):
        if self._devicename != "M5StickV":
            return
        if init:
            axp192 = pmu.axp192()
            axp192.setScreenBrightness(8)
            lcd.init(freq=40000000)
            lcd.direction(lcd.YX_LRUD)
        lcd.clear(lcd.BLACK)
        lcd.draw_string(10, 10, "CameraServerV", lcd.WHITE, lcd.BLACK)

App().main()
