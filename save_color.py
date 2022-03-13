import numpy as np
import cv2
import os

import OpenEXR
import Imath


def binaryToEXR(file_name, exr_name, rows, cols, layer):
    print("file_name:{}".format(file_name))
    if layer in ['velocity']:
        color = np.fromfile(file_name, dtype=np.float16, count=-1, sep='')
    elif layer in ['input', 'output', 'input_post']:
        color = np.fromfile(file_name, dtype=np.float16, count=-1, sep='')
    elif layer in ['depth']:
        color = np.fromfile(file_name, dtype=np.float32, count=-1, sep='')
    else:
        print("error layer:{}".format(layer))
    
    color = np.float16(color)
    color = color.reshape((rows, cols, -1))
    print("layer:{}, color.shape:{}, color.dtype:{}".format(layer, color.shape, color.dtype))
    
    if layer == 'depth':
        color = np.squeeze(color)
        type_chan = Imath.Channel(Imath.PixelType(Imath.PixelType.HALF))
        HEADER = OpenEXR.Header(color.shape[1], color.shape[0])
        HEADER['channels'] = dict([(c, type_chan) for c in "R"])
        exr = OpenEXR.OutputFile(exr_name, HEADER)
        # color = color / np.max(color) * 255
        exr.writePixels({"R": np.squeeze(color)})
        exr.close()
    elif layer == "velocity":
        type_chan = Imath.Channel(Imath.PixelType(Imath.PixelType.HALF))
        HEADER = OpenEXR.Header(color.shape[1], color.shape[0])
        HEADER['channels'] = dict([(c, type_chan) for c in "GR"])
        exr = OpenEXR.OutputFile(exr_name, HEADER)
        exr.writePixels({"G": np.squeeze(color[:,:,0]/1), "R":np.squeeze(color[:,:,1]/1)})
        exr.close()
    elif layer in ["input", "output", "input_post"]:
        type_chan = Imath.Channel(Imath.PixelType(Imath.PixelType.HALF))
        HEADER = OpenEXR.Header(color.shape[1], color.shape[0])
        HEADER['channels'] = dict([(c, type_chan) for c in "RGB"])
        exr = OpenEXR.OutputFile(exr_name, HEADER)
        exr.writePixels({"R": np.squeeze(color[:,:,0]/1), "G":np.squeeze(color[:,:,1]/1), "B":np.squeeze(color[:,:,2]/1)})
        exr.close()


if __name__ == "__main__":

    start = 1
    end = start + 460
    rows1 = 720
    cols1 = 1280
    rows2 = 720
    cols2 = 1280


    root_path = "e:/DLSS/data/TAA"
    folder_path = "03_13_18_06"
    input_root = "{}/raw/{}".format(root_path, folder_path)
    output_root = "{}/exr/{}".format(root_path, folder_path)
    os.makedirs(output_root, exist_ok=True)


    # layers = ['output', 'input']
    # layers = ['input']
    layers = ['input_post']
    for layer in layers:
        for count in range(start, end):
            print(layer)
            if layer == "velocity":
                rows = rows2
                cols = cols2
            elif layer == "output":
                rows = rows2
                cols = cols2
            elif layer == "input":
                rows = rows1
                cols = cols1
            elif layer == "input_post":
                rows = rows1
                cols = cols1
            file_name = "{}/{}_{}_{}_{}.txt".format(input_root, count, cols, rows, layer)
            exr_name = "{}/{}_{}_{}_{}.exr".format(output_root, count, cols, rows, layer)
            binaryToEXR(file_name, exr_name, rows, cols, layer)
