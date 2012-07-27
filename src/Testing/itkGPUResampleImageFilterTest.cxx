/*======================================================================

  This file is part of the elastix software.

  Copyright (c) University Medical Center Utrecht. All rights reserved.
  See src/CopyrightElastix.txt or http://elastix.isi.uu.nl/legal.php for
  details.

     This software is distributed WITHOUT ANY WARRANTY; without even
     the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
     PURPOSE. See the above copyright notices for more information.

======================================================================*/

// GPU include files
#include "itkGPUResampleImageFilter.h"
#include "itkGPUAffineTransform.h"
#include "itkGPUNearestNeighborInterpolateImageFunction.h"
#include "itkGPULinearInterpolateImageFunction.h"
#include "itkGPUBSplineInterpolateImageFunction.h"
#include "itkGPUBSplineDecompositionImageFilter.h"
#include "itkGPUExplicitSynchronization.h"

#include "itkImageFileReader.h"
#include "itkImageFileWriter.h"
#include "itkImageRegionConstIterator.h"
#include "itkMersenneTwisterRandomVariateGenerator.h"

#include "itkTimeProbe.h"
#include "itkOpenCLUtil.h" // IsGPUAvailable()
#include <iomanip> // setprecision, etc.

#include "itkCommandLineArgumentParser.h"


/**
* ******************* GetHelpString *******************
*/

std::string GetHelpString( void )
{
  std::stringstream ss;
  ss << "Usage:" << std::endl
    << "itkGPUResampleImageFilterAffineTransformTest" << std::endl
    << "  -in           input file name\n"
    << "  -out          output file names.(outputCPU outputGPU)\n"
    << "  [-i]          interpolator, one of {NearestNeighbor, Linear, BSpline}, default NearestNeighbor\n"
    << "  [-t]          transform, one of {Affine, BSpline}, default Affine\n";
  return ss.str();

} // end GetHelpString()


//------------------------------------------------------------------------------
// This test compares the CPU with the GPU version of the ResampleImageFilter.
// The filter takes an input image and produces an output image.
// We compare the CPU and GPU output image wrt RMSE and speed.

int main( int argc, char * argv[] )
{
  // Check for GPU
  if( !itk::IsGPUAvailable() )
  {
    std::cerr << "ERROR: OpenCL-enabled GPU is not present." << std::endl;
    return EXIT_FAILURE;
  }

  // Create a command line argument parser
  itk::CommandLineArgumentParser::Pointer parser = itk::CommandLineArgumentParser::New();
  parser->SetCommandLineArguments( argc, argv );
  parser->SetProgramHelpText( GetHelpString() );

  parser->MarkArgumentAsRequired( "-in", "The input filename" );
  parser->MarkArgumentAsRequired( "-out", "The output filenames" );

  itk::CommandLineArgumentParser::ReturnValue validateArguments = parser->CheckForRequiredArguments();

  if( validateArguments == itk::CommandLineArgumentParser::FAILED )
  {
    return EXIT_FAILURE;
  }
  else if( validateArguments == itk::CommandLineArgumentParser::HELPREQUESTED )
  {
    return EXIT_SUCCESS;
  }

  // Get command line arguments
  std::string inputFileName = "";
  const bool retin = parser->GetCommandLineArgument( "-in", inputFileName );

  std::vector<std::string> outputFileNames( 2, "" );
  parser->GetCommandLineArgument( "-out", outputFileNames );

  // interpolator argument
  std::string interp = "NearestNeighbor";
  parser->GetCommandLineArgument( "-i", interp );

  if( interp != "NearestNeighbor"
    && interp != "Linear"
    && interp != "BSpline" )
  {
    std::cerr << "ERROR: interpolator \"-i\" should be one of {NearestNeighbor, Linear, BSpline}." << std::endl;
    return EXIT_FAILURE;
  }

  // transform argument
  std::string trans = "Affine";
  parser->GetCommandLineArgument( "-t", trans );

  if( trans != "Affine"
    && trans != "BSpline" )
  {
    std::cerr << "ERROR: transform \"-t\" should be one of {Affine, BSpline}." << std::endl;
    return EXIT_FAILURE;
  }

  std::string parametersFileName = "";
  if( trans == "BSpline" )
  {
    const bool retp = parser->GetCommandLineArgument( "-p", parametersFileName );
    if(!retp)
    {
      std::cerr << "ERROR: You should specify parameters file \"-p\" for BSpline transform." << std::endl;
      return EXIT_FAILURE;
    }
  }

  const unsigned int splineOrderInterpolator = 3;
  const double epsilon = 0.03; //1e-3;
  const unsigned int runTimes = 5; // 5

  std::cout << std::showpoint << std::setprecision( 4 );

  // Typedefs.
  const unsigned int  Dimension = 3;
  typedef short       InputPixelType;
  typedef short       OutputPixelType;
  typedef itk::Image<InputPixelType, Dimension>   InputImageType;
  typedef itk::Image<OutputPixelType, Dimension>  OutputImageType;
  typedef InputImageType::SizeType::SizeValueType SizeValueType;

  // CPU typedefs
  typedef float       InterpolatorPrecisionType;
  typedef float       ScalarType;
  typedef itk::ResampleImageFilter<
    InputImageType, OutputImageType, InterpolatorPrecisionType>       FilterType;

  // Transform typedefs
  typedef itk::Transform<ScalarType, Dimension, Dimension>            TransformType;
  typedef itk::AffineTransform<ScalarType, Dimension>                 AffineTransformType;
  typedef itk::BSplineTransform<ScalarType, Dimension, 3>             BSplineTransformType;

  // Interpolate typedefs
  typedef itk::InterpolateImageFunction<
    InputImageType, InterpolatorPrecisionType>                        InterpolatorType;
  typedef itk::NearestNeighborInterpolateImageFunction<
    InputImageType, InterpolatorPrecisionType>                        NearestNeighborInterpolatorType;
  typedef itk::LinearInterpolateImageFunction<
    InputImageType, InterpolatorPrecisionType>                        LinearInterpolatorType;
  typedef itk::BSplineInterpolateImageFunction<
    InputImageType, ScalarType, InterpolatorPrecisionType>            BSplineInterpolatorType;

  typedef itk::ImageFileReader<InputImageType>  ReaderType;
  typedef itk::ImageFileWriter<OutputImageType> WriterType;

  // Reader
  ReaderType::Pointer reader = ReaderType::New();
  reader->SetFileName( inputFileName );
  reader->Update();

  //double spacing[ Dimension ];
  //for( unsigned int i = 0; i < Dimension; i++ )
  //{
  //  spacing[i] = 1.0;
  //}

  // Construct and setup the resample filter
  FilterType::Pointer filter = FilterType::New();

  typedef itk::Statistics::MersenneTwisterRandomVariateGenerator RandomNumberGeneratorType;
  RandomNumberGeneratorType::Pointer randomNum = RandomNumberGeneratorType::GetInstance();

  InputImageType::ConstPointer  inputImage     = reader->GetOutput();
  InputImageType::SpacingType   inputSpacing   = inputImage->GetSpacing();
  InputImageType::PointType     inputOrigin    = inputImage->GetOrigin();
  InputImageType::DirectionType inputDirection = inputImage->GetDirection();
  InputImageType::RegionType    inputRegion    = inputImage->GetBufferedRegion();
  InputImageType::SizeType      inputSize      = inputRegion.GetSize();

  OutputImageType::SpacingType    outputSpacing;
  OutputImageType::PointType      outputOrigin;
  OutputImageType::DirectionType  outputDirection;
  OutputImageType::SizeType       outputSize;
  for( unsigned int i = 0; i < Dimension; i++ )
  {
    double tmp = randomNum->GetUniformVariate( 0.9 , 1.1 );
    outputSpacing[ i ] = inputSpacing[ i ] * tmp;

    tmp = randomNum->GetUniformVariate( 0.9 , 1.1 );
    outputOrigin[ i ] = inputOrigin[ i ] * tmp;

    for( unsigned int j = 0; j < Dimension; j++ )
    {
      //tmp = randomNum->GetUniformVariate( 0.9 * inputOrigin[ i ], 1.1 * inputOrigin[ i ] );
      outputDirection[ i ][ j ] = inputDirection[ i ][ j ];// * tmp;
    }

    tmp = randomNum->GetUniformVariate( 0.9 , 1.1 );
    outputSize[ i ] = itk::Math::Round<SizeValueType>( inputSize[ i ] * tmp );
  }

  filter->SetDefaultPixelValue( -1.0 );
  filter->SetOutputSpacing( outputSpacing );
  filter->SetOutputOrigin( outputOrigin );
  filter->SetOutputDirection( outputDirection );
  filter->SetSize( outputSize );
  filter->SetOutputStartIndex( inputRegion.GetIndex() );

  // Construct, select and setup transform
  TransformType::Pointer transform;
  TransformType::ParametersType parameters;

  typedef BSplineTransformType::MeshSizeType MeshSizeType;
  MeshSizeType meshSize;
  typedef BSplineTransformType::PhysicalDimensionsType PhysicalDimensionsType;
  PhysicalDimensionsType fixedDimensions;

  if( trans == "Affine" )
  {
    AffineTransformType::Pointer tmpTransform
      = AffineTransformType::New();

    // Setup parameters
    parameters.SetSize( Dimension * Dimension + Dimension );
    unsigned int par = 0;
    if( Dimension == 2 )
    {
      // matrix part
      parameters[ par++ ] = 0.9;
      parameters[ par++ ] = 0.1;
      parameters[ par++ ] = 0.2;
      parameters[ par++ ] = 1.1;
      // translation
      parameters[ par++ ] = 0.0;
      parameters[ par++ ] = 0.0;
    }
    else if( Dimension == 3 )
    {
      // matrix part
      parameters[ par++ ] = 1.03;
      parameters[ par++ ] = 0.2;
      parameters[ par++ ] = 0.0;
      parameters[ par++ ] = -0.21;
      parameters[ par++ ] = 1.12;
      parameters[ par++ ] = 0.3;
      parameters[ par++ ] = 0.0;
      parameters[ par++ ] = 0.01;
      parameters[ par++ ] = 0.8;
      // translation
      parameters[ par++ ] = -10.0;
      parameters[ par++ ] = 5.1;
      parameters[ par++ ] = 0.0;
    }

    // assign parameters
    transform = tmpTransform;
  }
  else if( trans == "BSpline" )
  {
    BSplineTransformType::Pointer tmpTransform
      = BSplineTransformType::New();

    // Setup parameters
    meshSize.Fill( 4 );

    for(unsigned int d=0; d<Dimension; d++)
    {
      fixedDimensions[d] = inputSpacing[d] * ( inputSize[d] - 1.0 );
    }

    tmpTransform->SetTransformDomainOrigin( inputOrigin );
    tmpTransform->SetTransformDomainDirection( inputDirection );
    tmpTransform->SetTransformDomainPhysicalDimensions( fixedDimensions );
    tmpTransform->SetTransformDomainMeshSize( meshSize );

    const unsigned int numberOfParameters = tmpTransform->GetNumberOfParameters();
    parameters.SetSize( numberOfParameters );

    std::ifstream infile;
    infile.open( parametersFileName.c_str() );

    const unsigned int numberOfNodes = numberOfParameters / Dimension;
    for( unsigned int n=0; n < numberOfNodes; n++ )
    {
      unsigned int parValue;
      infile >> parValue;
      parameters[n] = parValue;
      if(Dimension > 1)
        parameters[n+numberOfNodes] = parValue;
      if(Dimension > 2)
        parameters[n+numberOfNodes*2] = parValue;
    }
    infile.close();

    // assign parameters
    transform = tmpTransform;
  }
  transform->SetParameters( parameters );

  // Construct, select and setup interpolator
  InterpolatorType::Pointer interpolator;
  if( interp == "NearestNeighbor" )
  {
    NearestNeighborInterpolatorType::Pointer tmpInterpolator
      = NearestNeighborInterpolatorType::New();
    interpolator = tmpInterpolator;
  }
  else if( interp == "Linear" )
  {
    LinearInterpolatorType::Pointer tmpInterpolator
      = LinearInterpolatorType::New();
    interpolator = tmpInterpolator;
  }
  else if( interp == "BSpline" )
  {
    BSplineInterpolatorType::Pointer tmpInterpolator
      = BSplineInterpolatorType::New();
    tmpInterpolator->SetSplineOrder( splineOrderInterpolator );
    interpolator = tmpInterpolator;
  }

  //
  std::cout << "Testing the ResampleImageFilter, CPU vs GPU:\n";
  std::cout << "CPU/GPU transform interpolator #threads time RMSE\n";

  // Time the filter, run on the CPU
  itk::TimeProbe cputimer;
  cputimer.Start();

  for( unsigned int i = 0; i < runTimes; i++ )
  {
    filter->SetInput( reader->GetOutput() );
    filter->SetTransform( transform );
    filter->SetInterpolator( interpolator );
    try{ filter->Update(); }
    catch( itk::ExceptionObject & e )
    {
      std::cerr << "ERROR: " << e << std::endl;
      return EXIT_FAILURE;
    }

    if(runTimes > 1)
    {
      filter->Modified();
    }
  }
  cputimer.Stop();

  std::cout << "CPU " << transform->GetNameOfClass()
    << " " << interpolator->GetNameOfClass()
    << " " << filter->GetNumberOfThreads()
    << " " << cputimer.GetMean() / runTimes << std::endl;

  /** Write the CPU result. */
  WriterType::Pointer writer = WriterType::New();
  writer->SetInput( filter->GetOutput() );
  writer->SetFileName( outputFileNames[ 0 ].c_str() );
  try{ writer->Update(); }
  catch( itk::ExceptionObject & e )
  {
    std::cerr << "ERROR: " << e << std::endl;
    return EXIT_FAILURE;
  }

  // Register object factory for GPU image and filter
  // All these filters that are constructed after this point are
  // turned into a GPU filter.
  itk::ObjectFactoryBase::Pointer imageFactory = NULL;
  imageFactory = itk::GPUImageFactory::New();
  itk::ObjectFactoryBase::RegisterFactory( imageFactory );
  itk::ObjectFactoryBase::RegisterFactory( itk::GPUResampleImageFilterFactory::New() );
  itk::ObjectFactoryBase::RegisterFactory( itk::GPUCastImageFilterFactory::New() );

  if( trans == "Affine")
  {
    itk::ObjectFactoryBase::RegisterFactory( itk::GPUAffineTransformFactory::New() );
  }
  else if( trans == "BSpline" )
  {
    itk::ObjectFactoryBase::RegisterFactory( itk::GPUBSplineTransformFactory::New() );
  }

  if( interp == "NearestNeighbor" )
  {
    itk::ObjectFactoryBase::RegisterFactory( itk::GPUNearestNeighborInterpolateImageFunctionFactory::New() );
  }
  else if( interp == "Linear" )
  {
    itk::ObjectFactoryBase::RegisterFactory( itk::GPULinearInterpolateImageFunctionFactory::New() );
  }
  else if( interp == "BSpline" )
  {
    itk::ObjectFactoryBase::RegisterFactory( itk::GPUBSplineInterpolateImageFunctionFactory::New() );
    itk::ObjectFactoryBase::RegisterFactory( itk::GPUBSplineDecompositionImageFilterFactory::New() );
  }

  // Construct the filter
  // Use a try/catch, because construction of this filter will trigger
  // OpenCL compilation, which may fail.
  FilterType::Pointer gpuFilter;
  try{ gpuFilter = FilterType::New(); }
  catch( itk::ExceptionObject & e )
  {
    std::cerr << "ERROR: " << e << std::endl;
    return EXIT_FAILURE;
  }

  gpuFilter->SetDefaultPixelValue( -1.0 );
  gpuFilter->SetOutputSpacing( outputSpacing );
  gpuFilter->SetOutputOrigin( outputOrigin );
  gpuFilter->SetOutputDirection( outputDirection );
  gpuFilter->SetSize( outputSize );
  gpuFilter->SetOutputStartIndex( inputRegion.GetIndex() );

  // Also need to re-construct the image reader, so that it now
  // reads a GPUImage instead of a normal image.
  // Otherwise, you will get an exception when running the GPU filter:
  // "ERROR: The GPU InputImage is NULL. Filter unable to perform."
  ReaderType::Pointer gpuReader = ReaderType::New();
  gpuReader->SetFileName( inputFileName );
  gpuReader->Update(); // needed?

  // Setup GPU transform
  TransformType::Pointer gpuTransform;

  if( trans == "Affine" )
  {
    AffineTransformType::Pointer tmpTransform
      = AffineTransformType::New();
    gpuTransform = tmpTransform;
  }
  else if( trans == "BSpline" )
  {
    BSplineTransformType::Pointer tmpTransform
      = BSplineTransformType::New();

    tmpTransform->SetTransformDomainOrigin( inputOrigin );
    tmpTransform->SetTransformDomainDirection( inputDirection );
    tmpTransform->SetTransformDomainPhysicalDimensions( fixedDimensions );
    tmpTransform->SetTransformDomainMeshSize( meshSize );

    gpuTransform = tmpTransform;
  }
  gpuTransform->SetParameters( parameters );

  // Construct, select and setup interpolator
  InterpolatorType::Pointer gpuInterpolator;
  if( interp == "NearestNeighbor" )
  {
    NearestNeighborInterpolatorType::Pointer tmpInterpolator
      = NearestNeighborInterpolatorType::New();
    gpuInterpolator = tmpInterpolator;
  }
  else if( interp == "Linear" )
  {
    LinearInterpolatorType::Pointer tmpInterpolator
      = LinearInterpolatorType::New();
    gpuInterpolator = tmpInterpolator;
  }
  else if( interp == "BSpline" )
  {
    BSplineInterpolatorType::Pointer tmpInterpolator
      = BSplineInterpolatorType::New();
    tmpInterpolator->SetSplineOrder( splineOrderInterpolator );
    gpuInterpolator = tmpInterpolator;
  }

  // Time the filter, run on the GPU
  itk::TimeProbe gputimer;
  gputimer.Start();
  for( unsigned int i = 0; i < runTimes; i++ )
  {
    gpuFilter->SetInput( gpuReader->GetOutput() );
    gpuFilter->SetTransform( gpuTransform );
    gpuFilter->SetInterpolator( gpuInterpolator );
    try{ gpuFilter->Update(); }
    catch( itk::ExceptionObject & e )
    {
      std::cerr << "ERROR: " << e << std::endl;
      return EXIT_FAILURE;
    }
    // Due to some bug in the ITK synchronisation we now manually
    // copy the result from GPU to CPU, without calling Update() again,
    // and not clearing GPU memory afterwards.
    itk::GPUExplicitSync<FilterType, OutputImageType>( gpuFilter, false, false );
    //itk::GPUExplicitSync<FilterType, ImageType>( gpuFilter, false, true ); // crashes!

    if(runTimes > 1)
    {
      gpuFilter->Modified();
    }
  }
  // GPU buffer has not been copied yet, so we have to make manual update
  //itk::GPUExplicitSync<FilterType, ImageType>( gpuFilter, false, true );
  gputimer.Stop();

  std::cout << "GPU " << transform->GetNameOfClass()
    << " " << interpolator->GetNameOfClass()
    << " x " << gputimer.GetMean() / runTimes;

  /** Write the GPU result. */
  WriterType::Pointer gpuWriter = WriterType::New();
  gpuWriter->SetInput( gpuFilter->GetOutput() );
  gpuWriter->SetFileName( outputFileNames[ 1 ].c_str() );
  try{ gpuWriter->Update(); }
  catch( itk::ExceptionObject & e )
  {
    std::cerr << "ERROR: " << e << std::endl;
    return EXIT_FAILURE;
  }

  // UnRegister GPUImage before using BinaryThresholdImageFilter,
  // Otherwise GPU memory will be allocated
  itk::ObjectFactoryBase::UnRegisterFactory( imageFactory );

  // Compute RMSE
  itk::ImageRegionConstIterator<OutputImageType> cit(
    filter->GetOutput(), filter->GetOutput()->GetLargestPossibleRegion() );
  itk::ImageRegionConstIterator<OutputImageType> git(
    gpuFilter->GetOutput(), gpuFilter->GetOutput()->GetLargestPossibleRegion() );

  double rmse = 0.0;
  for( cit.GoToBegin(), git.GoToBegin(); !cit.IsAtEnd(); ++cit, ++git )
  {
    double err = static_cast<double>( cit.Get() ) - static_cast<double>( git.Get() );
    rmse += err * err;
  }
  rmse = vcl_sqrt( rmse / filter->GetOutput()->GetLargestPossibleRegion().GetNumberOfPixels() );
  std::cout << " " << rmse << std::endl;

  // Check
  if( rmse > epsilon )
  {
    std::cerr << "ERROR: RMSE between CPU and GPU result larger than expected" << std::endl;
    return EXIT_FAILURE;
  }

  // End program.
  return EXIT_SUCCESS;

}
