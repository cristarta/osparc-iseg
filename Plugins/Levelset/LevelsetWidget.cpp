/*
 * Copyright (c) 2018 The Foundation for Research on Information Technologies in Society (IT'IS).
 * 
 * This file is part of iSEG
 * (see https://github.com/ITISFoundation/osparc-iseg).
 * 
 * This software is released under the MIT License.
 *  https://opensource.org/licenses/MIT
 */
#include "LevelsetWidget.h"

#include "Data/ItkUtils.h"
#include "Data/SliceHandlerItkWrapper.h"

#include <itkImage.h>
#include <itkBinaryThresholdImageFilter.h>
#include <itkFastMarchingImageFilter.h>
#include <itkThresholdSegmentationLevelSetImageFilter.h>
#include <itkConstNeighborhoodIterator.h>
#include <itkImageFileWriter.h>

#include <QFormLayout>

#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/max.hpp>
#include <boost/accumulators/statistics/min.hpp>
#include <boost/accumulators/statistics/variance.hpp>
#include <accumulators/percentile.hpp>

#include <algorithm>
#include <sstream>

namespace
{
	template<class T> void dump_image(T *img, const std::string& file_name)
	{
		auto writer = itk::ImageFileWriter<T>::New();
		writer->SetInput(img);
		writer->SetFileName(file_name);
		writer->Update();
	}
}

namespace acc = boost::accumulators;

LevelsetWidget::LevelsetWidget(iseg::SliceHandlerInterface* hand3D, QWidget* parent,
		const char* name, Qt::WindowFlags wFlags)
		: WidgetInterface(parent, name, wFlags), handler3D(hand3D)
{
	activeslice = handler3D->active_slice();

	setToolTip(Format("LevelSetSegmentation: (Pick with OLC Foreground 1 pixel to start)"));

	all_slices = new QCheckBox;

	iterations = new QSpinBox(1, 50000, 1, nullptr);
	iterations->setValue(1200);
	iterations->setToolTip(Format(""));

	lower_threshold = new QLineEdit(QString::number(0.0));
	lower_threshold->setValidator(new QDoubleValidator);

	upper_threshold = new QLineEdit(QString::number(1.0));
	upper_threshold->setValidator(new QDoubleValidator);

	multiplier = new QLineEdit(QString::number(2.5));
	multiplier->setValidator(new QDoubleValidator);
	multiplier->setToolTip(Format(
		"Used to estimate thresholds."
		"The confidence interval is the mean plus or minus the 'Multiplier' times the standard deviation."));

	guess_threshold = new QPushButton("Estimate thresholds");

	execute_button = new QPushButton("Execute");

	auto layout = new QFormLayout;
	layout->addRow(QString("Apply to all slices"), all_slices);
	layout->addRow(QString("Iterations"), iterations);
	layout->addRow(QString("Lower threshold"), lower_threshold);
	layout->addRow(QString("Upper threshold"), upper_threshold);
	layout->addRow(QString("Multiplier"), multiplier);
	layout->addRow(guess_threshold);
	layout->addRow(execute_button);
	setLayout(layout);

	QObject::connect(guess_threshold, SIGNAL(clicked()), this, SLOT(guess_thresholds()));
	QObject::connect(execute_button, SIGNAL(clicked()), this, SLOT(do_work()));
}

void LevelsetWidget::init()
{
	on_slicenr_changed();
	hideparams_changed();
}

void LevelsetWidget::newloaded()
{
	clearmarks();
	on_slicenr_changed();
}

void LevelsetWidget::on_slicenr_changed()
{
	activeslice = handler3D->active_slice();
}

void LevelsetWidget::cleanup()
{
	clearmarks();
}

void LevelsetWidget::clearmarks()
{
	vpdyn.clear();

	std::vector<iseg::Point> empty;
	emit vpdyn_changed(&empty);
}

void LevelsetWidget::on_mouse_clicked(iseg::Point p)
{
	vpdyn[activeslice].push_back(p);

	if (!all_slices->isChecked())
	{
		//do_work();
	}
}

void LevelsetWidget::get_seeds(std::vector<itk::Index<2>>& seeds)
{
	auto vp = vpdyn[activeslice];
	for (auto p : vp)
	{
		itk::Index<2> idx;
		idx[0] = p.px;
		idx[1] = p.py;
		seeds.push_back(idx);
	}
}

void LevelsetWidget::get_seeds(std::vector<itk::Index<3>>& seeds)
{
	auto start_slice = handler3D->start_slice();
	for (auto slice : vpdyn)
	{
		for (auto p : slice.second)
		{
			itk::Index<3> idx;
			idx[0] = p.px;
			idx[1] = p.py;
			idx[2] = slice.first - start_slice;
			seeds.push_back(idx);
		}
	}
}

void LevelsetWidget::guess_thresholds()
{
	iseg::SliceHandlerItkWrapper itk_handler(handler3D);
	if (all_slices->isChecked())
	{
		using input_type = itk::SliceContiguousImage<float>;
		auto source = itk_handler.GetImage(iseg::SliceHandlerItkWrapper::kSource, true);
		guess_thresholds_nd<input_type>(source);
	}
	else
	{
		using input_type = itk::Image<float, 2>;
		auto source = itk_handler.GetImageSlice(iseg::SliceHandlerItkWrapper::kSource);
		guess_thresholds_nd<input_type>(source);
	}
}

template<class TInput>
void LevelsetWidget::guess_thresholds_nd(TInput* source)
{
	using input_type = TInput;

	std::vector<typename input_type::IndexType> indices;
	get_seeds(indices);

	input_type::SizeType radius;
	radius.Fill(2);

	acc::accumulator_set<double, acc::features<
		acc::tag::mean,
		acc::tag::min,
		acc::tag::max,
		//acc::tag::percentile,
		acc::tag::variance
	> > stats;

	itk::ConstNeighborhoodIterator<input_type> it(radius, source, source->GetLargestPossibleRegion());
	size_t const N = it.Size();
	for (auto idx : indices)
	{
		it.SetLocation(idx);

		for (size_t i = 0; i < N; i++)
		{
			bool in_bounds;
			double v = it.GetPixel(i, in_bounds);
			if (in_bounds)
			{
				stats(v);
			}
		}
	}

	auto min = acc::min(stats);
	auto max = acc::max(stats);
	auto mean = acc::mean(stats);
	auto stddev = std::sqrt(acc::variance(stats));
	auto s = multiplier->text().toDouble();
	lower_threshold->setText(QString::number(mean - s * stddev));
	upper_threshold->setText(QString::number(mean + s * stddev));
}

void LevelsetWidget::do_work()
{
	iseg::SliceHandlerItkWrapper itk_handler(handler3D);
	if (all_slices->isChecked())
	{
		using input_type = itk::SliceContiguousImage<float>;
		auto source = itk_handler.GetImage(iseg::SliceHandlerItkWrapper::kSource, true);
		auto target = itk_handler.GetImage(iseg::SliceHandlerItkWrapper::kTarget, true);
		do_work_nd<input_type>(source, target);
	}
	else
	{
		using input_type = itk::Image<float, 2>;
		auto source = itk_handler.GetImageSlice(iseg::SliceHandlerItkWrapper::kSource);
		auto target = itk_handler.GetImageSlice(iseg::SliceHandlerItkWrapper::kTarget);
		do_work_nd<input_type>(source, target);
	}
}

template<typename TInput>
void LevelsetWidget::do_work_nd(TInput* input, TInput* target)
{
	itkStaticConstMacro(ImageDimension, unsigned int, TInput::ImageDimension);
	using input_type = TInput;
	using real_type = itk::Image<float, ImageDimension>;
	using mask_type = itk::Image<unsigned char, ImageDimension>;

	using fast_marching_type = itk::FastMarchingImageFilter<real_type, real_type>;
	using node_container_type = typename fast_marching_type::NodeContainer;
	using node_type = typename fast_marching_type::NodeType;

	// get seeds
	std::vector<typename input_type::IndexType> indices;
	get_seeds(indices);

	// create filters
	auto fast_marching = fast_marching_type::New();
	auto threshold_levelset = itk::ThresholdSegmentationLevelSetImageFilter<real_type, input_type>::New();
	auto threshold = itk::BinaryThresholdImageFilter<real_type, mask_type>::New();

	// setup pipeline
	threshold_levelset->SetInput(fast_marching->GetOutput());
	threshold_levelset->SetFeatureImage(input);
	threshold->SetInput(threshold_levelset->GetOutput());

	// setup seeds
	const double initialDistance = 2.0; // \todo BL
	const double seedValue = -initialDistance; // \todo BL
	auto seeds = node_container_type::New();
	seeds->Initialize();
	for (size_t i = 0; i<indices.size(); ++i)
	{
		node_type node;
		node.SetValue(seedValue);
		node.SetIndex(indices[i]);
		seeds->InsertElement(i, node);
	}

	// set parameters
	fast_marching->SetTrialPoints(seeds);
	fast_marching->SetSpeedConstant(1.0);
	fast_marching->SetOutputRegion(input->GetBufferedRegion());
	fast_marching->SetOutputSpacing(input->GetSpacing());
	fast_marching->SetOutputOrigin(input->GetOrigin());
	fast_marching->SetOutputDirection(input->GetDirection());

	threshold_levelset->SetPropagationScaling(1.0); 
	threshold_levelset->SetCurvatureScaling(1.0);
	threshold_levelset->SetMaximumRMSError(0.02);
	threshold_levelset->SetNumberOfIterations(iterations->value());
	threshold_levelset->SetUpperThreshold(lower_threshold->text().toDouble());
	threshold_levelset->SetLowerThreshold(upper_threshold->text().toDouble());
	threshold_levelset->SetIsoSurfaceValue(0.0);

	threshold->SetLowerThreshold(-5000.0); // \todo BL
	threshold->SetUpperThreshold(0);
	threshold->SetOutsideValue(0);
	threshold->SetInsideValue(255);

	try
	{
		threshold->Update();

		dump_image(threshold_levelset->GetOutput(), "E:/temp/_ls_levelset.nii.gz");
		dump_image(threshold->GetOutput(), "E:/temp/_ls_final.nii.gz");
	}
	catch (itk::ExceptionObject e)
	{
		std::cerr << "Error: " << e.what() << "\n";
		return;
	}

	iseg::DataSelection dataSelection;
	dataSelection.allSlices = all_slices->isChecked();
	dataSelection.sliceNr = activeslice;
	dataSelection.work = true;
	emit begin_datachange(dataSelection, this);

	if (!iseg::Paste<mask_type, input_type>(threshold->GetOutput(), target))
	{
		std::cerr << "Error: could not set output because image regions don't match.\n";
	}

	emit end_datachange(this);
}
