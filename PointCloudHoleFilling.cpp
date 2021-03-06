/*=========================================================================
 *
 *  Copyright David Doria 2012 daviddoria@gmail.com
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *         http://www.apache.org/licenses/LICENSE-2.0.txt
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 *
 *=========================================================================*/

// STL
#include <sstream>

// Submodules
#include <Helpers/Helpers.h>
#include <Mask/Mask.h>
#include <ITKHelpers/ITKHelpers.h>

// PoissonEditing
#include <PoissonEditing/PoissonEditing.h>

// PTXTools
#include <PTXTools/PTXReader.h>

// PatchBasedInpainting
#include <PatchBasedInpainting/ImageProcessing/Derivatives.h>
#include <PatchBasedInpainting/Drivers/LidarInpaintingHSVTextureVerification.hpp>
#include <PatchBasedInpainting/Drivers/LidarInpaintingRGBTextureVerification.hpp>

// SmallHoleFiller
#include <SmallHoleFiller/SmallHoleFiller.h>

// ITK
#include "itkImage.h"
#include "itkImageFileReader.h"

// Note: The mask image should appear upside down if viewed with a standard image viewer (i.e. gimp, etc). This is because the (0,0) pixel in the
// scan grid is in the lower left corner (the scans are taken in columns bottom to top, left to right), but the image coordinate system has (0,0) in
// the top left.
int main(int argc, char *argv[])
{
  // Verify arguments
  if(argc != 5)
  {
    std::cerr << "Required arguments: PointCloud.ptx imageMask.mask patchHalfWidth outputPrefix" << std::endl;
    std::cerr << "Input arguments: ";
    for(int i = 1; i < argc; ++i)
    {
      std::cerr << argv[i] << " ";
    }
    return EXIT_FAILURE;
  }

  // Parse arguments
  std::string ptxFileName = argv[1];
  std::string maskFileName = argv[2];

  std::stringstream ssPatchHalfWidth;
  ssPatchHalfWidth << argv[3];
  unsigned int patchHalfWidth = 0;
  ssPatchHalfWidth >> patchHalfWidth;

  std::string outputPrefix = argv[4];

  // Output arguments
  std::cout << "Reading ptx: " << ptxFileName << std::endl;
  std::cout << "Reading mask: " << maskFileName << std::endl;
  std::cout << "Patch half width: " << patchHalfWidth << std::endl;
  std::cout << "Output prefix: " << outputPrefix << std::endl;

  // Read the files
  PTXImage ptxImage = PTXReader::Read(ptxFileName);

  Mask::Pointer mask = Mask::New();
  mask->Read(maskFileName);

  // We need this because the 'mask' above gets filled during the inpainting
  Mask::Pointer originalMask = Mask::New();
  originalMask->DeepCopyFrom(mask);

  if(mask->GetLargestPossibleRegion() != ptxImage.GetFullRegion())
  {
    std::stringstream ss;
    ss << "PTX and mask must be the same size! PTX is " << ptxImage.GetFullRegion()
       << " and mask is " << mask->GetLargestPossibleRegion();
    throw std::runtime_error(ss.str());
  }

  ptxImage.WritePointCloud(std::string("Original.vtp"));

  ///////////// Fill invalid pixels in the PTX grid /////////////

  // Find the invalid pixels
  PTXImage::MaskImageType::Pointer invalidMaskImage = PTXImage::MaskImageType::New();
  ptxImage.CreateValidityImage(invalidMaskImage);
  Mask::Pointer invalidMask = Mask::New();
  PTXImage::MaskImageType::PixelType holeValue = 0;
  invalidMask->CreateFromImage(invalidMaskImage.GetPointer(), holeValue);

  PTXImage::RGBDImageType::Pointer rgbdImage = PTXImage::RGBDImageType::New();
  ptxImage.CreateRGBDImage(rgbdImage.GetPointer());
  ITKHelpers::WriteImage(rgbdImage.GetPointer(), "RGBD.mha");

  SmallHoleFiller<PTXImage::RGBDImageType> smallHoleFiller(rgbdImage.GetPointer(), invalidMask);
  //smallHoleFiller.SetWriteIntermediateOutput(true);
  unsigned int kernelRadius = 1;
  unsigned int downsampleFactor = 1;
  smallHoleFiller.SetKernelRadius(kernelRadius);
  smallHoleFiller.SetDownsampleFactor(downsampleFactor);
  smallHoleFiller.Fill();

  ITKHelpers::WriteImage(smallHoleFiller.GetOutput(), "Valid.mha");

  ptxImage.SetAllPointsToValid(); // This call must come before ReplaceRGBD, because the values are only replaced for valid pixels!

  ptxImage.ReplaceRGBD(smallHoleFiller.GetOutput());

//  ptxImage.WritePTX(std::string("Valid.ptx"));

  ptxImage.WritePointCloud(std::string("Valid.vtp"));

  ///////////// Inpaint the specified hole /////////////
  typedef PTXImage::DepthImageType DepthImageType;
  DepthImageType::Pointer depthImage = DepthImageType::New();
  ptxImage.CreateDepthImage(depthImage);

  typedef itk::Image<itk::CovariantVector<float, 2>, 2> GradientImageType;
  GradientImageType::Pointer depthGradientImage = GradientImageType::New();

  // This assumes that the hole has been defined such that the hole boundary is not close enough to the object being inpainted for those pixels
  // to contribute to the computation. That is, if the mask was specified by a segmentation for example, it should be dilated before using this program
  // because the gradients computed by ForwardDifferenceDerivatives will be erroneous near the hole boundary.
  // This must be used rather than something like MaskedGradient because the Poisson equation needs to use the same operator as was used in the derivative computations.
  ITKHelpers::ForwardDifferenceDerivatives(depthImage.GetPointer(), depthGradientImage.GetPointer());

  typedef PTXImage::RGBImageType RGBImageType;
  RGBImageType::Pointer rgbImage = RGBImageType::New();
  ptxImage.CreateRGBImage(rgbImage);

  // Construct RGBDxDy image to inpaint
  typedef itk::Image<itk::CovariantVector<float, 5>, 2> RGBDxDyImageType;
  RGBDxDyImageType::Pointer rgbDxDyImage = RGBDxDyImageType::New();
  ITKHelpers::StackImages(rgbImage.GetPointer(), depthGradientImage.GetPointer(), rgbDxDyImage.GetPointer());
  ITKHelpers::WriteImage(rgbDxDyImage.GetPointer(), "RGBDxDy.mha");

  // Fill the hole with black so that if debugging images are output they are easier to interpret
  RGBDxDyImageType::PixelType zeroPixel;
  zeroPixel.Fill(0);
  zeroPixel[0] = 255; // make the pixel red if interpreted as RGB
  originalMask->ApplyToImage(rgbDxDyImage.GetPointer(), zeroPixel);

  // Inpaint
  const unsigned int numberOfKNN = 100;
//  const unsigned int numberOfKNN = 500;

  float slightBlurVariance = 0.0f;

  float reduction = .7;
  unsigned int imageRadius = rgbDxDyImage->GetLargestPossibleRegion().GetSize()[0]/2;
  unsigned int searchRadius = imageRadius * reduction;

  // Search the region that is the same size as the image, but centered at the patch. For the very center patch, this will search the whole image.
  //  unsigned int searchRadius = rgbDxDyImage->GetLargestPossibleRegion().GetSize()[0]/2;

  // Use a region twice the size of the image, this should always include (almost) all of the image in the search region.
  // Since only the [0] dimension is used, if the image is not square we could still miss parts of image edge when searching around patches near the edge of the image.
//  unsigned int searchRadius = rgbDxDyImage->GetLargestPossibleRegion().GetSize()[0];

//  float localRegionSizeMultiplier = 1.5f;
  float localRegionSizeMultiplier = 4.0f;
  float maxAllowedUsedPixelsRatio = 0.5f;
  LidarInpaintingHSVTextureVerification(rgbDxDyImage.GetPointer(), mask, patchHalfWidth,
                                        numberOfKNN, slightBlurVariance, searchRadius, localRegionSizeMultiplier, maxAllowedUsedPixelsRatio);
//  LidarInpaintingRGBTextureVerification(rgbDxDyImage.GetPointer(), mask, patchHalfWidth,
//                                        numberOfKNN, slightBlurVariance, searchRadius);

  ITKHelpers::WriteImage(rgbDxDyImage.GetPointer(), "InpaintedRGBDxDy.mha");

  ///////////// Assemble the result /////////////
  // Extract inpainted depth gradients
  std::vector<unsigned int> depthGradientChannels = {3, 4};
  GradientImageType::Pointer inpaintedDepthGradients = GradientImageType::New();
  ITKHelpers::ExtractChannels(rgbDxDyImage.GetPointer(), depthGradientChannels,
                              inpaintedDepthGradients.GetPointer());
  ITKHelpers::WriteImage(inpaintedDepthGradients.GetPointer(), "InpaintedDepthGradients.mha");

  // Extract inpainted RGB image
  std::vector<unsigned int> rgbChannels = {0, 1, 2};
  RGBImageType::Pointer inpaintedRGBImage = RGBImageType::New();
  ITKHelpers::ExtractChannels(rgbDxDyImage.GetPointer(), rgbChannels,
                              inpaintedRGBImage.GetPointer());
  ITKHelpers::WriteImage(inpaintedRGBImage.GetPointer(), "InpaintedRGB.png");

  // Poisson filling (we have to use the 'originalMask' because 'mask' will have been completely filled (no more hole pixels) during the inpainting)
  DepthImageType::Pointer inpaintedDepthImage = DepthImageType::New();
  PoissonEditing<float>::FillScalarImage(depthImage.GetPointer(), originalMask,
                                         inpaintedDepthGradients.GetPointer(),
                                         inpaintedDepthImage.GetPointer());
  ITKHelpers::WriteImage(inpaintedDepthImage.GetPointer(), "ReconstructedDepth.mha");

  // Assemble and write output
  PTXImage filledPTX = ptxImage;
  filledPTX.SetAllPointsToValid();
  filledPTX.ReplaceDepth(inpaintedDepthImage);
  filledPTX.ReplaceRGB(inpaintedRGBImage);

  std::stringstream ssPTXOutput;
  ssPTXOutput << outputPrefix << ".ptx";
  filledPTX.WritePTX(ssPTXOutput.str());

  std::stringstream ssVTPOutput;
  ssVTPOutput << outputPrefix << ".vtp";
  filledPTX.WritePointCloud(ssVTPOutput.str());

  return EXIT_SUCCESS;
}
