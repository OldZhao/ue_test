from unicodedata import name
import numpy
import OpenEXR
import Imath
from pip import main

def read_exr(exrfile):
    file = OpenEXR.InputFile(exrfile)
    print("header:{}".format(file.header()))
    for k in file.header():
        print("key:{}:{}".format(k, file.header()[k]))
    print("channel:{}".format(file.channel))

    pt = Imath.PixelType(Imath.PixelType.FLOAT)

    dw = file.header()['dataWindow']
    size = (dw.max.x - dw.min.x + 1, dw.max.y - dw.min.y + 1)

    rgbf = [Image.fromstring("F", size, file.channel(c, pt)) for c in "RGB"]



if __name__ == "__main__":
    exrfile = r"D:\pc_code\DLSS\UE_project\TD_1205\Saved\MovieRenders\Scene_1_02.931338.exr"
    read_exr(exrfile)

