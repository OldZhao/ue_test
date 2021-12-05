struct DepthPixel	//定义深度像素结构体
		{
			float depth;
			char stencil;
			char unused1;
			char unused2;
			char unused3;
		};

		float* cpuDataPtr;	// Texture深度值数组首地址
		TArray<DepthPixel> mydata;	//最终获取色深度值数据
		FIntPoint buffsize;	//深度长宽大小X和Y

		ENQUEUE_RENDER_COMMAND(ReadSurfaceFloatCommand)(	// 将读取深度数据的命令推给渲染线程进行执行
			[&cpuDataPtr, &mydata, &buffsize](FRHICommandListImmediate& RHICmdList) //&cpuDataPtr, &mydata, &buffsize为传入的外部参数
			{
				FSceneRenderTargets::Get(RHICmdList).AdjustGBufferRefCount(RHICmdList, 1);
				FTexture2DRHIRef uTex2DRes = FSceneRenderTargets::Get(RHICmdList).GetSceneDepthSurface();
				buffsize = uTex2DRes->GetSizeXY();
				uint32 sx = buffsize.X;
				uint32 sy = buffsize.Y;
				mydata.AddUninitialized(sx * sy);
				uint32 Lolstrid = 0;
				cpuDataPtr = (float*)RHILockTexture2D(uTex2DRes, 0, RLM_ReadOnly, Lolstrid, true);	// 加锁 获取可读depth Texture深度值数组首地址
				memcpy(mydata.GetData(), cpuDataPtr, sx * sy * sizeof(DepthPixel));		//复制深度数据
				RHIUnlockTexture2D(uTex2DRes, 0, true);	//解锁
				FSceneRenderTargets::Get(RHICmdList).AdjustGBufferRefCount(RHICmdList, -1);

				std::string PathRoot = "D:/pc_code/data/TAA/map_DLSS_" + std::to_string(count) + "_" + std::to_string(sx) + "_" + std::to_string(sy);
				std::string Filename = PathRoot + "_depth.txt";
				int bytes = sx * sy * 8;
				std::ofstream b_stream(Filename.c_str(), std::fstream::out | std::fstream::binary);
				if (b_stream) {
					b_stream.write((char*)mydata.GetData(), bytes);
				}
				b_stream.close();
			});
