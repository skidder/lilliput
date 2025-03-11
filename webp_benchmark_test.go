package lilliput

import (
	"fmt"
	"os"
	"path/filepath"
	"testing"
	"time"
)

type encoderConfig struct {
	method         int // 0-6, where 0 is fastest and 6 is best compression
	quality        int // 0-100
	filterStrength int // 0-100
	filterType     int // 0-1
	autofilter     int // 0-1
	partitions     int // 0-3
	segments       int // 1-4
	preprocessing  int // 0-1
	threads        int // 1-n
	palette        int // 0-1
}

func (c encoderConfig) String() string {
	return fmt.Sprintf("m%d_q%d_fs%d_ft%d_af%d_p%d_s%d_pp%d_t%d_pl%d",
		c.method, c.quality, c.filterStrength, c.filterType,
		c.autofilter, c.partitions, c.segments, c.preprocessing, c.threads, c.palette)
}

type benchmarkResult struct {
	config     encoderConfig
	duration   time.Duration
	outputSize int64
	psnr       float64
}

var testConfigs = []encoderConfig{
	// Ultra fast encoding - WebP optimized
	{method: 0, quality: 60, filterStrength: 20, filterType: 0, autofilter: 0, partitions: 0, segments: 1, preprocessing: 0, threads: 1, palette: 0},
	{method: 1, quality: 60, filterStrength: 20, filterType: 0, autofilter: 0, partitions: 0, segments: 1, preprocessing: 0, threads: 1, palette: 0},

	// Ultra fast encoding - GIF optimized (with palette)
	{method: 0, quality: 60, filterStrength: 20, filterType: 0, autofilter: 0, partitions: 0, segments: 1, preprocessing: 0, threads: 1, palette: 1},

	// Fast encoding, lower quality
	{method: 0, quality: 75, filterStrength: 20, filterType: 0, autofilter: 0, partitions: 0, segments: 1, preprocessing: 0, threads: 1, palette: 0},
	{method: 1, quality: 75, filterStrength: 20, filterType: 0, autofilter: 0, partitions: 0, segments: 1, preprocessing: 0, threads: 1, palette: 0},

	// Fast encoding - GIF optimized (with palette)
	{method: 0, quality: 75, filterStrength: 20, filterType: 0, autofilter: 0, partitions: 0, segments: 1, preprocessing: 0, threads: 1, palette: 1},

	// Intermediate method (2)
	{method: 2, quality: 75, filterStrength: 30, filterType: 0, autofilter: 0, partitions: 0, segments: 1, preprocessing: 0, threads: 1, palette: 0},

	// Balanced encoding
	{method: 3, quality: 80, filterStrength: 40, filterType: 1, autofilter: 1, partitions: 1, segments: 2, preprocessing: 1, threads: 1, palette: 0},
	{method: 4, quality: 80, filterStrength: 40, filterType: 1, autofilter: 1, partitions: 1, segments: 2, preprocessing: 1, threads: 1, palette: 0},

	// High quality encoding
	{method: 5, quality: 90, filterStrength: 60, filterType: 1, autofilter: 1, partitions: 2, segments: 3, preprocessing: 1, threads: 1, palette: 0},
	{method: 6, quality: 90, filterStrength: 60, filterType: 1, autofilter: 1, partitions: 2, segments: 3, preprocessing: 1, threads: 1, palette: 0},

	// Segment count variations (using method 4 as baseline)
	{method: 4, quality: 80, filterStrength: 40, filterType: 1, autofilter: 1, partitions: 1, segments: 1, preprocessing: 1, threads: 1, palette: 0},
	{method: 4, quality: 80, filterStrength: 40, filterType: 1, autofilter: 1, partitions: 1, segments: 3, preprocessing: 1, threads: 1, palette: 0},
	{method: 4, quality: 80, filterStrength: 40, filterType: 1, autofilter: 1, partitions: 1, segments: 4, preprocessing: 1, threads: 1, palette: 0},
}

func BenchmarkWebPEncoding(b *testing.B) {
	testCases := []struct {
		name      string
		inputPath string
	}{
		{"AnimatedWebP", "testdata/animated-webp-supported.webp"},
		{"AnimatedGIF", "testdata/big_buck_bunny_720_5s.gif"},
	}

	for _, tc := range testCases {
		b.Run(tc.name, func(b *testing.B) {
			// Read input file
			inputData, err := os.ReadFile(tc.inputPath)
			if err != nil {
				b.Fatalf("Failed to read input file: %v", err)
			}

			// Create output directory if it doesn't exist
			outDir := filepath.Join("testdata", "benchmark_out")
			if err := os.MkdirAll(outDir, 0755); err != nil {
				b.Fatalf("Failed to create output directory: %v", err)
			}

			for _, config := range testConfigs {
				b.Run(config.String(), func(b *testing.B) {
					var decoder Decoder
					var err error

					// Create appropriate decoder based on input type
					if filepath.Ext(tc.inputPath) == ".gif" {
						decoder, err = newGifDecoder(inputData)
					} else {
						decoder, err = newWebpDecoder(inputData)
					}
					if err != nil {
						b.Fatalf("Failed to create decoder: %v", err)
					}
					// defer decoder.Close()

					// Get original dimensions
					header, err := decoder.Header()
					if err != nil {
						b.Fatalf("Failed to get header: %v", err)
					}

					options := &ImageOptions{
						FileType:             ".webp",
						NormalizeOrientation: true,
						Width:                header.Width(),
						Height:               header.Height(),
						ResizeMethod:         ImageOpsNoResize,
						EncodeTimeout:        time.Second * 300,
						EncodeOptions: map[int]int{
							WebpQuality:        config.quality,
							WebpMethod:         config.method,
							WebpFilterStrength: config.filterStrength,
							WebpFilterType:     config.filterType,
							WebpAutofilter:     config.autofilter,
							WebpPartitions:     config.partitions,
							WebpSegments:       config.segments,
							WebpPreprocessing:  config.preprocessing,
							WebpThreadLevel:    config.threads,
							WebpPalette:        config.palette,
						},
					}

					dstBuf := make([]byte, destinationBufferSize)
					ops := NewImageOps(8192)
					defer ops.Close()

					b.ResetTimer()
					var lastOutput []byte

					for i := 0; i < b.N; i++ {
						output, err := ops.Transform(decoder, options, dstBuf)
						if err != nil {
							b.Fatalf("Transform failed: %v", err)
						}
						lastOutput = output

						// Reset decoder for next iteration
						decoder.Close()
						if filepath.Ext(tc.inputPath) == ".gif" {
							decoder, _ = newGifDecoder(inputData)
						} else {
							decoder, _ = newWebpDecoder(inputData)
						}
					}

					b.StopTimer()

					// Save the last output for analysis
					outPath := filepath.Join(outDir, fmt.Sprintf("%s_%s.webp",
						filepath.Base(tc.inputPath), config.String()))
					if err := os.WriteFile(outPath, lastOutput, 0644); err != nil {
						b.Fatalf("Failed to write output file: %v", err)
					}

					// Record results
					fileInfo, err := os.Stat(outPath)
					if err != nil {
						b.Fatalf("Failed to get output file info: %v", err)
					}

					result := benchmarkResult{
						config:     config,
						duration:   b.Elapsed() / time.Duration(b.N),
						outputSize: fileInfo.Size(),
					}

					b.ReportMetric(float64(result.duration.Milliseconds()), "ms/op")
					b.ReportMetric(float64(result.outputSize), "output_size_bytes/op")
				})
			}
		})
	}
}
