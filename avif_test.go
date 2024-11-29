package lilliput

import (
	"os"
	"reflect"
	"testing"
	"time"
)

func TestAvifOperations(t *testing.T) {
	t.Run("NewAvifDecoder", testNewAvifDecoder)
	t.Run("AvifDecoder_Header", testAvifDecoderHeader)
	t.Run("NewAvifEncoder", testNewAvifEncoder)
	t.Run("AvifDecoder_DecodeTo", testAvifDecoderDecodeTo)
	t.Run("AvifEncoder_Encode", testAvifEncoderEncode)
	t.Run("AvifToWebP_Conversion", testAvifToWebPConversion)
	t.Run("NewAvifDecoderWithAnimatedSource", testNewAvifDecoderWithAnimatedSource)
	t.Run("NewAvifEncoderWithWebPAnimatedSource", testNewAvifEncoderWithWebPAnimatedSource)
	t.Run("NewAvifEncoderWithVideoSource", testNewAvifEncoderWithVideoSource)

}

func testNewAvifDecoder(t *testing.T) {
	testAvifImage, err := os.ReadFile("testdata/colors_sdr_srgb.avif")
	if err != nil {
		t.Fatalf("Unexpected error while reading AVIF image: %v", err)
	}
	decoder, err := newAvifDecoder(testAvifImage)
	if err != nil {
		t.Fatalf("Unexpected error while decoding AVIF image data: %v", err)
	}
	defer decoder.Close()
}

func testAvifDecoderHeader(t *testing.T) {
	testAvifImage, err := os.ReadFile("testdata/colors_sdr_srgb.avif")
	if err != nil {
		t.Fatalf("Unexpected error while reading AVIF image: %v", err)
	}
	decoder, err := newAvifDecoder(testAvifImage)
	if err != nil {
		t.Fatalf("Unexpected error while decoding AVIF image data: %v", err)
	}
	defer decoder.Close()

	header, err := decoder.Header()
	if err != nil {
		t.Fatalf("Unexpected error: %v", err)
	}
	if reflect.TypeOf(header).String() != "*lilliput.ImageHeader" {
		t.Fatalf("Expected type *lilliput.ImageHeader, got %v", reflect.TypeOf(header))
	}
}

func testNewAvifEncoder(t *testing.T) {
	testCases := []struct {
		name     string
		filename string
	}{
		{"No ICC Profile", "testdata/colors_sdr_srgb.avif"},
		{"With ICC Profile", "testdata/paris_icc_exif_xmp.avif"},
	}

	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			testAvifImage, err := os.ReadFile(tc.filename)
			if err != nil {
				t.Fatalf("Unexpected error while reading AVIF image: %v", err)
			}
			decoder, err := newAvifDecoder(testAvifImage)
			if err != nil {
				t.Fatalf("Unexpected error while decoding AVIF image data: %v", err)
			}
			defer decoder.Close()

			dstBuf := make([]byte, destinationBufferSize)
			encoder, err := newAvifEncoder(decoder, dstBuf)
			if err != nil {
				t.Fatalf("Unexpected error: %v", err)
			}
			defer encoder.Close()
		})
	}
}

func testAvifDecoderDecodeTo(t *testing.T) {
	testCases := []struct {
		name     string
		filename string
	}{
		{"No ICC Profile", "testdata/colors_sdr_srgb.avif"},
		{"With ICC Profile", "testdata/paris_icc_exif_xmp.avif"},
	}

	for _, tc := range testCases {

		t.Run(tc.name, func(t *testing.T) {
			testAvifImage, err := os.ReadFile(tc.filename)
			if err != nil {
				t.Fatalf("Failed to read AVIF image: %v", err)
			}
			decoder, err := newAvifDecoder(testAvifImage)
			if err != nil {
				t.Fatalf("Failed to create a new AVIF decoder: %v", err)
			}
			defer decoder.Close()

			header, err := decoder.Header()
			if err != nil {
				t.Fatalf("Failed to get the header: %v", err)
			}
			framebuffer := NewFramebuffer(header.width, header.height)
			if err = decoder.DecodeTo(framebuffer); err != nil {
				t.Errorf("DecodeTo failed unexpectedly: %v", err)
			}
		})
	}

	t.Run("Invalid Framebuffer", func(t *testing.T) {
		testAvifImage, _ := os.ReadFile("testdata/colors_sdr_srgb.avif")
		decoder, _ := newAvifDecoder(testAvifImage)
		defer decoder.Close()

		if err := decoder.DecodeTo(nil); err == nil {
			t.Error("DecodeTo with nil framebuffer should fail, but it did not")
		}
	})
}

func testAvifEncoderEncode(t *testing.T) {
	testCases := []struct {
		name     string
		filename string
		quality  int
	}{
		{"No ICC Profile", "testdata/colors_sdr_srgb.avif", 60},
		{"With ICC Profile", "testdata/paris_icc_exif_xmp.avif", 80},
	}

	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			testAvifImage, err := os.ReadFile(tc.filename)
			if err != nil {
				t.Fatalf("Failed to read AVIF image: %v", err)
			}

			decoder, err := newAvifDecoder(testAvifImage)
			if err != nil {
				t.Fatalf("Failed to create a new AVIF decoder: %v", err)
			}
			defer decoder.Close()

			header, err := decoder.Header()
			if err != nil {
				t.Fatalf("Failed to get the header: %v", err)
			}
			framebuffer := NewFramebuffer(header.width, header.height)
			if err = framebuffer.resizeMat(header.width, header.height, header.pixelType); err != nil {
				t.Fatalf("Failed to resize the framebuffer: %v", err)
			}

			dstBuf := make([]byte, destinationBufferSize)
			encoder, err := newAvifEncoder(decoder, dstBuf)
			if err != nil {
				t.Fatalf("Failed to create a new AVIF encoder: %v", err)
			}
			defer encoder.Close()

			options := map[int]int{AvifQuality: tc.quality, AvifSpeed: 10}
			encodedData, err := encoder.Encode(framebuffer, options)
			if err != nil {
				t.Fatalf("Encode failed unexpectedly: %v", err)
			}
			if encodedData, err = encoder.Encode(nil, options); err != nil {
				t.Fatalf("Encode of empty frame failed unexpectedly: %v", err)
			}
			if len(encodedData) == 0 {
				t.Fatalf("Encoded data is empty, but it should not be")
			}
		})
	}
}

func testAvifToWebPConversion(t *testing.T) {
	testCases := []struct {
		name         string
		inputPath    string
		outputPath   string
		width        int
		height       int
		quality      int
		resizeMethod ImageOpsSizeMethod
	}{
		{
			name:         "AVIF to WebP conversion with no ICC Profile",
			inputPath:    "testdata/colors_sdr_srgb.avif",
			outputPath:   "testdata/out/colors_sdr_srgb_converted.webp",
			width:        100,
			height:       100,
			quality:      80,
			resizeMethod: ImageOpsFit,
		},
		{
			name:         "AVIF to WebP conversion with ICC Profile",
			inputPath:    "testdata/paris_icc_exif_xmp.avif",
			outputPath:   "testdata/out/paris_icc_exif_xmp_converted.webp",
			width:        200,
			height:       150,
			quality:      60,
			resizeMethod: ImageOpsFit,
		},
	}

	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			testAvifImage, err := os.ReadFile(tc.inputPath)
			if err != nil {
				t.Fatalf("Failed to read AVIF image: %v", err)
			}

			decoder, err := newAvifDecoder(testAvifImage)
			if err != nil {
				t.Fatalf("Failed to create AVIF decoder: %v", err)
			}
			defer decoder.Close()

			dstBuf := make([]byte, destinationBufferSize)
			options := &ImageOptions{
				FileType:             ".webp",
				NormalizeOrientation: true,
				EncodeOptions:        map[int]int{WebpQuality: tc.quality},
				ResizeMethod:         tc.resizeMethod,
				Width:                tc.width,
				Height:               tc.height,
				EncodeTimeout:        time.Second * 30,
			}

			ops := NewImageOps(2000)
			defer ops.Close()

			newDst, err := ops.Transform(decoder, options, dstBuf)
			if err != nil {
				t.Fatalf("Transform failed: %v", err)
			}

			if len(newDst) == 0 {
				t.Fatal("Transform returned empty data")
			}

			if tc.outputPath != "" {
				if _, err := os.Stat("testdata/out"); os.IsNotExist(err) {
					if err = os.Mkdir("testdata/out", 0755); err != nil {
						t.Fatalf("Failed to create output directory: %v", err)
					}
				}

				if err = os.WriteFile(tc.outputPath, newDst, 0644); err != nil {
					t.Fatalf("Failed to write output file: %v", err)
				}
			}
		})
	}
}

func testNewAvifDecoderWithAnimatedSource(t *testing.T) {
	testCases := []struct {
		name         string
		inputPath    string
		outputPath   string
		width        int
		height       int
		quality      int
		resizeMethod ImageOpsSizeMethod
		outputType   string
	}{
		{
			name:         "Animated AVIF to WebP encoding",
			inputPath:    "testdata/colors-animated-8bpc-alpha-exif-xmp.avif",
			outputPath:   "testdata/out/animated_sample_out.webp",
			width:        100,
			height:       100,
			quality:      60,
			resizeMethod: ImageOpsFit,
			outputType:   ".webp",
		},
		{
			name:         "Animated AVIF to WebP encoding",
			inputPath:    "testdata/colors-animated-8bpc-alpha-exif-xmp.avif",
			outputPath:   "testdata/out/animated_sample_out.avif",
			width:        100,
			height:       100,
			quality:      60,
			resizeMethod: ImageOpsFit,
			outputType:   ".avif",
		},
	}

	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			testAvifImage, err := os.ReadFile(tc.inputPath)
			if err != nil {
				t.Fatalf("Failed to read AVIF image: %v", err)
			}

			decoder, err := newAvifDecoder(testAvifImage)
			if err != nil {
				t.Fatalf("Failed to create AVIF decoder: %v", err)
			}
			defer decoder.Close()

			dstBuf := make([]byte, destinationBufferSize)
			options := &ImageOptions{
				FileType:              tc.outputType,
				NormalizeOrientation:  true,
				EncodeOptions:         map[int]int{WebpQuality: tc.quality},
				ResizeMethod:          tc.resizeMethod,
				Width:                 tc.width,
				Height:                tc.height,
				EncodeTimeout:         time.Second * 30,
				DisableAnimatedOutput: false,
			}

			ops := NewImageOps(2000)
			defer ops.Close()

			newDst, err := ops.Transform(decoder, options, dstBuf)
			if err != nil {
				t.Fatalf("Transform failed: %v", err)
			}

			if len(newDst) == 0 {
				t.Fatal("Transform returned empty data")
			}

			if tc.outputPath != "" {
				if _, err := os.Stat("testdata/out"); os.IsNotExist(err) {
					if err = os.Mkdir("testdata/out", 0755); err != nil {
						t.Fatalf("Failed to create output directory: %v", err)
					}
				}

				if err = os.WriteFile(tc.outputPath, newDst, 0644); err != nil {
					t.Fatalf("Failed to write output file: %v", err)
				}
			}
		})
	}
}

func testNewAvifEncoderWithWebPAnimatedSource(t *testing.T) {
	testCases := []struct {
		name                  string
		inputPath             string
		outputPath            string
		width                 int
		height                int
		quality               int
		resizeMethod          ImageOpsSizeMethod
		outputType            string
		disableAnimatedOutput bool
	}{
		{
			name:                  "Animated WebP to Animated AVIF encoding",
			inputPath:             "testdata/animated-webp-supported.webp",
			outputPath:            "testdata/out/animated-webp-supported_out_fit.avif",
			width:                 400,
			height:                400,
			quality:               95,
			resizeMethod:          ImageOpsFit,
			outputType:            ".avif",
			disableAnimatedOutput: false,
		},
		{
			name:                  "Animated WebP to Animated AVIF encoding",
			inputPath:             "testdata/animated-webp-supported.webp",
			outputPath:            "testdata/out/animated-webp-supported_out_fit_still.avif",
			width:                 100,
			height:                100,
			quality:               60,
			resizeMethod:          ImageOpsFit,
			outputType:            ".avif",
			disableAnimatedOutput: true,
		},
		{
			name:                  "Animated WebP to Animated AVIF encoding",
			inputPath:             "testdata/animated-webp-supported.webp",
			outputPath:            "testdata/out/animated-webp-supported_out_fit_high_quality_still.avif",
			width:                 400,
			height:                400,
			quality:               80,
			resizeMethod:          ImageOpsFit,
			outputType:            ".avif",
			disableAnimatedOutput: true,
		},
	}

	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			testWebpImage, err := os.ReadFile(tc.inputPath)
			if err != nil {
				t.Fatalf("Failed to read WebP image: %v", err)
			}

			decoder, err := newWebpDecoder(testWebpImage)
			if err != nil {
				t.Fatalf("Failed to create WebP decoder: %v", err)
			}
			defer decoder.Close()

			dstBuf := make([]byte, destinationBufferSize)
			options := &ImageOptions{
				FileType:              tc.outputType,
				NormalizeOrientation:  true,
				EncodeOptions:         map[int]int{AvifQuality: tc.quality, AvifSpeed: 10},
				ResizeMethod:          tc.resizeMethod,
				Width:                 tc.width,
				Height:                tc.height,
				EncodeTimeout:         time.Second * 30,
				DisableAnimatedOutput: tc.disableAnimatedOutput,
			}

			ops := NewImageOps(2000)
			defer ops.Close()

			newDst, err := ops.Transform(decoder, options, dstBuf)
			if err != nil {
				t.Fatalf("Transform failed: %v", err)
			}

			if len(newDst) == 0 {
				t.Fatal("Transform returned empty data")
			}

			if tc.outputPath != "" {
				if _, err := os.Stat("testdata/out"); os.IsNotExist(err) {
					if err = os.Mkdir("testdata/out", 0755); err != nil {
						t.Fatalf("Failed to create output directory: %v", err)
					}
				}

				if err = os.WriteFile(tc.outputPath, newDst, 0644); err != nil {
					t.Fatalf("Failed to write output file: %v", err)
				}
			}
		})
	}
}

func testNewAvifEncoderWithVideoSource(t *testing.T) {
	testCases := []struct {
		name                  string
		inputPath             string
		outputPath            string
		outputType            string
		quality               int
		resizeMethod          ImageOpsSizeMethod
		width                 int
		height                int
		disableAnimatedOutput bool
	}{
		{
			name:                  "MP4 Video to Resized AVIF",
			inputPath:             "testdata/big_buck_bunny_480p_10s_std.mp4",
			outputPath:            "testdata/out/video_resized.avif",
			outputType:            ".avif",
			quality:               AvifQuality,
			resizeMethod:          ImageOpsFit,
			width:                 320,
			height:                240,
			disableAnimatedOutput: true,
		},
	}

	for _, tc := range testCases {
		t.Run(tc.name, func(t *testing.T) {
			testVideoData, err := os.ReadFile(tc.inputPath)
			if err != nil {
				t.Fatalf("Failed to read video data: %v", err)
			}

			decoder, err := newAVCodecDecoder(testVideoData)
			if err != nil {
				t.Fatalf("Failed to create AVCodec decoder: %v", err)
			}
			defer decoder.Close()

			dstBuf := make([]byte, destinationBufferSize)
			options := &ImageOptions{
				FileType:              tc.outputType,
				NormalizeOrientation:  true,
				EncodeOptions:         map[int]int{AvifQuality: tc.quality, AvifSpeed: 10},
				ResizeMethod:          tc.resizeMethod,
				Width:                 tc.width,
				Height:                tc.height,
				EncodeTimeout:         time.Second * 30,
				DisableAnimatedOutput: tc.disableAnimatedOutput,
			}

			ops := NewImageOps(2000)
			defer ops.Close()

			newDst, err := ops.Transform(decoder, options, dstBuf)
			if err != nil {
				t.Fatalf("Transform failed: %v", err)
			}

			if len(newDst) == 0 {
				t.Fatal("Transform returned empty data")
			}

			if tc.outputPath != "" {
				if _, err := os.Stat("testdata/out"); os.IsNotExist(err) {
					if err = os.Mkdir("testdata/out", 0755); err != nil {
						t.Fatalf("Failed to create output directory: %v", err)
					}
				}

				if err = os.WriteFile(tc.outputPath, newDst, 0644); err != nil {
					t.Fatalf("Failed to write output file: %v", err)
				}
			}
		})
	}
}
