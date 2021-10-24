import numpy as np
import cv2

import OpenEXR
import Imath


def binaryToEXR(file_name, exr_name, rows, cols, layer):
    if layer in ['velocity']:
        color = np.fromfile(file_name, dtype=np.float16, count=-1, sep='')
    elif layer in ['input', 'output']:
        color = np.fromfile(file_name, dtype=np.float16, count=-1, sep='')
    elif layer in ['depth']:
        color = np.fromfile(file_name, dtype=np.float32, count=-1, sep='')
    else:
        print("error layer:{}".format(layer))
    
    color = np.float32(color)
    color = color.reshape((rows, cols, -1))
    print("layer:{}, color.shape:{}, color.dtype:{}".format(layer, color.shape, color.dtype))
    
    if layer == 'depth':
        color = np.squeeze(color)
        type_chan = Imath.Channel(Imath.PixelType(Imath.PixelType.FLOAT))
        HEADER = OpenEXR.Header(color.shape[1], color.shape[0])
        HEADER['channels'] = dict([(c, type_chan) for c in "R"])
        exr = OpenEXR.OutputFile(exr_name, HEADER)
        color = color / np.max(color) * 255
        exr.writePixels({"R": np.squeeze(color)})
        exr.close()
    elif layer == "velocity":
        type_chan = Imath.Channel(Imath.PixelType(Imath.PixelType.FLOAT))
        HEADER = OpenEXR.Header(color.shape[1], color.shape[0])
        HEADER['channels'] = dict([(c, type_chan) for c in "GR"])
        exr = OpenEXR.OutputFile(exr_name, HEADER)
        exr.writePixels({"G": np.squeeze(color[:,:,0]/1), "R":np.squeeze(color[:,:,1]/1)})
        exr.close()
    elif layer in ["input", "output"]:
        type_chan = Imath.Channel(Imath.PixelType(Imath.PixelType.FLOAT))
        HEADER = OpenEXR.Header(color.shape[1], color.shape[0])
        HEADER['channels'] = dict([(c, type_chan) for c in "RGB"])
        exr = OpenEXR.OutputFile(exr_name, HEADER)
        exr.writePixels({"R": np.squeeze(color[:,:,0]/1), "G":np.squeeze(color[:,:,1]/1), "B":np.squeeze(color[:,:,2]/1)})
        exr.close()


if __name__ == "__main__":

    start = 20
    end = start + 10
    for count in range(start, end):
        layers = ['output', 'input']
        for layer in layers:
            print(layer)
            if layer == "velocity":
                rows = 702
                cols = 1171
            elif layer == "output":
                rows = 564
                cols = 1072
            elif layer == "input":
                rows = 282
                cols = 536
            file_name = "d:/pc_code/data/map_DLSS_{}_{}_{}_{}.txt".format(count, cols, rows, layer)
            exr_name = file_name.replace(".txt", ".exr")
            binaryToEXR(file_name, exr_name, rows, cols, layer)
