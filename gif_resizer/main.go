package main

import (
	"flag"
	"fmt"
	"os"
	"path/filepath"
	"time"

	"github.com/discord/lilliput"
)

var EncodeOptions = map[string]map[int]int{
	".gif": {},
}

func main() {
	var inputFilename string
	var outputWidth int
	var outputHeight int
	var outputFilename string

	flag.StringVar(&inputFilename, "input", "", "name of input GIF file to resize")
	flag.StringVar(&outputFilename, "output", "", "name of output file (optional)")
	flag.IntVar(&outputWidth, "width", 0, "width of output file")
	flag.IntVar(&outputHeight, "height", 0, "height of output file")
	flag.Parse()

	if inputFilename == "" {
		fmt.Printf("No input filename provided, quitting.\n")
		flag.Usage()
		os.Exit(1)
	}

	// Get input file size
	inputFileInfo, err := os.Stat(inputFilename)
	if err != nil {
		fmt.Printf("error getting input file info: %s\n", err)
		os.Exit(1)
	}
	fmt.Printf("Input file size: %d bytes\n", inputFileInfo.Size())

	inputBuf, err := os.ReadFile(inputFilename)
	if err != nil {
		fmt.Printf("failed to read input file, %s\n", err)
		os.Exit(1)
	}

	decoder, err := lilliput.NewDecoder(inputBuf)
	if err != nil {
		fmt.Printf("error decoding image, %s\n", err)
		os.Exit(1)
	}
	defer decoder.Close()

	// Verify it's a GIF
	if decoder.Description() != "GIF" {
		fmt.Printf("Input file must be a GIF, got %s\n", decoder.Description())
		os.Exit(1)
	}

	header, err := decoder.Header()
	if err != nil {
		fmt.Printf("error reading image header, %s\n", err)
		os.Exit(1)
	}

	// Print basic info about the GIF
	fmt.Printf("Animated GIF details:\n")
	fmt.Printf("Dimensions: %dpx x %dpx\n", header.Width(), header.Height())
	fmt.Printf("Duration: %.2f s\n", float64(decoder.Duration())/float64(time.Second))

	ops := lilliput.NewImageOps(8192)
	defer ops.Close()

	outputImg := make([]byte, 500*1024*1024)

	if outputWidth == 0 {
		outputWidth = header.Width()
	}
	if outputHeight == 0 {
		outputHeight = header.Height()
	}

	opts := &lilliput.ImageOptions{
		FileType:             ".gif",
		Width:                outputWidth,
		Height:               outputHeight,
		ResizeMethod:         lilliput.ImageOpsFit,
		NormalizeOrientation: true,
		EncodeOptions:        EncodeOptions[".gif"],
		EncodeTimeout:        60 * time.Second,
	}

	transformStartTime := time.Now()
	outputImg, err = ops.Transform(decoder, opts, outputImg)
	if err != nil {
		fmt.Printf("error transforming image, %s\n", err)
		os.Exit(1)
	}
	fmt.Printf("transformed in %s\n", time.Since(transformStartTime).String())

	if outputFilename == "" {
		outputFilename = "resized_" + filepath.Base(inputFilename)
	}

	if err = os.WriteFile(outputFilename, outputImg, 0644); err != nil {
		fmt.Printf("error writing out resized image, %s\n", err)
		os.Exit(1)
	}

	// Get output file size
	outputFileInfo, err := os.Stat(outputFilename)
	if err != nil {
		fmt.Printf("error getting output file info: %s\n", err)
		os.Exit(1)
	}
	fmt.Printf("Output file size: %d bytes\n", outputFileInfo.Size())
	fmt.Printf("Image written to %s\n", outputFilename)
}
