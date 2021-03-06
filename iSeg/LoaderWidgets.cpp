/*
 * Copyright (c) 2018 The Foundation for Research on Information Technologies in Society (IT'IS).
 * 
 * This file is part of iSEG
 * (see https://github.com/ITISFoundation/osparc-iseg).
 * 
 * This software is released under the MIT License.
 *  https://opensource.org/licenses/MIT
 */
#include "Precompiled.h"

#define cimg_display 0
#include "CImg.h"
#include "LoaderWidgets.h"
#include "XdmfImageReader.h"

#include "Interface/LayoutTools.h"
#include "Interface/RecentPlaces.h"

#include "Data/Point.h"
#include "Data/ScopedTimer.h"

#include "Core/ColorLookupTable.h"
#include "Core/ImageReader.h"
#include "Core/ImageWriter.h"

#include "Thirdparty/nanoflann/nanoflann.hpp"

#include <q3hbox.h>
#include <q3vbox.h>
#include <qbuttongroup.h>
#include <qfiledialog.h>
#include <qgridlayout.h>
#include <qgroupbox.h>
#include <qlabel.h>
#include <qlayout.h>
#include <qmessagebox.h>
#include <qpushbutton.h>
#include <qradiobutton.h>
#include <qstring.h>

#include <QMouseEvent>
#include <QPainter>

#include <boost/algorithm/string.hpp>
#include <boost/dll.hpp>
#include <boost/filesystem.hpp>

namespace {
template<class VectorOfVectorsType, typename num_t = double, int DIM = -1, class Distance = nanoflann::metric_L2, typename IndexType = size_t>
struct KDTreeVectorOfVectorsAdaptor
{
	using self_t = KDTreeVectorOfVectorsAdaptor<VectorOfVectorsType, num_t, DIM, Distance>;
	using metric_t = typename Distance::template traits<num_t, self_t>::distance_t;
	using index_t = nanoflann::KDTreeSingleIndexAdaptor<metric_t, self_t, DIM, IndexType>;

	index_t* index; //! The kd-tree index for the user to call its methods as usual with any other FLANN index.

	/// Constructor: takes a const ref to the vector of vectors object with the data points
	KDTreeVectorOfVectorsAdaptor(const size_t /* dimensionality */, const VectorOfVectorsType& mat, const int leaf_max_size = 10) : m_data(mat)
	{
		assert(mat.size() != 0 && mat[0].size() != 0);
		const size_t dims = mat[0].size();
		if (DIM > 0 && static_cast<int>(dims) != DIM)
			throw std::runtime_error("Data set dimensionality does not match the 'DIM' template argument");
		index = new index_t(static_cast<int>(dims), *this /* adaptor */, nanoflann::KDTreeSingleIndexAdaptorParams(leaf_max_size));
		index->buildIndex();
	}

	~KDTreeVectorOfVectorsAdaptor()
	{
		delete index;
	}

	const VectorOfVectorsType& m_data;

	/** Query for the \a num_closest closest points to a given point (entered as query_point[0:dim-1]).
        *  Note that this is a short-cut method for index->findNeighbors().
        *  The user can also call index->... methods as desired.
        * \note nChecks_IGNORED is ignored but kept for compatibility with the original FLANN interface.
        */
	inline void query(const num_t* query_point, const size_t num_closest, IndexType* out_indices, num_t* out_distances_sq, const int nChecks_IGNORED = 10) const
	{
		nanoflann::KNNResultSet<num_t, IndexType> resultSet(num_closest);
		resultSet.init(out_indices, out_distances_sq);
		index->findNeighbors(resultSet, query_point, nanoflann::SearchParams());
	}

	/** @name Interface expected by KDTreeSingleIndexAdaptor
        * @{ */

	const self_t& derived() const
	{
		return *this;
	}
	self_t& derived()
	{
		return *this;
	}

	// Must return the number of data points
	inline size_t kdtree_get_point_count() const
	{
		return m_data.size();
	}

	// Returns the dim'th component of the idx'th point in the class:
	inline num_t kdtree_get_pt(const size_t idx, const size_t dim) const
	{
		return m_data[idx][dim];
	}

	// Optional bounding-box computation: return false to default to a standard bbox computation loop.
	//   Return true if the BBOX was already computed by the class and returned in "bb" so it can be avoided to redo it again.
	//   Look at bb.size() to find out the expected dimensionality (e.g. 2 or 3 for point clouds)
	template<class BBOX>
	bool kdtree_get_bbox(BBOX& /*bb*/) const
	{
		return false;
	}

	/** @} */
};
} // namespace

namespace iseg {

namespace algo = boost::algorithm;
namespace fs = boost::filesystem;

ExportImg::ExportImg(SlicesHandler* h, QWidget* p, const char* n, Qt::WindowFlags f)
		: QDialog(p, n, f), handler3D(h)
{
	auto img_selection_hbox = new QHBoxLayout;
	img_selection_group = make_button_group({"Source", "Target", "Tissue"}, 0);
	for (auto b : img_selection_group->buttons())
	{
		img_selection_hbox->addWidget(b);
	}

	auto slice_selection_hbox = new QHBoxLayout;
	slice_selection_group = make_button_group({"Current Slice", "Active Slices"}, 0);
	for (auto b : slice_selection_group->buttons())
	{
		slice_selection_hbox->addWidget(b);
	}

	auto button_hbox = new QHBoxLayout;
	button_hbox->addWidget(pb_save = new QPushButton("OK"));
	button_hbox->addWidget(pb_cancel = new QPushButton("Cancel"));

	auto top_layout = new QVBoxLayout;
	top_layout->addLayout(img_selection_hbox);
	top_layout->addLayout(slice_selection_hbox);
	top_layout->addLayout(button_hbox);
	setLayout(top_layout);

	connect(pb_save, SIGNAL(clicked()), this, SLOT(save_pushed()));
	connect(pb_cancel, SIGNAL(clicked()), this, SLOT(close()));
}

void ExportImg::save_pushed()
{
	// todo: what to do about file series, e.g. select with some option (including base name), select directory (or save file name without extension, then append)
	QString filter =
			"Nifty file (*.nii.gz *nii.gz)\n"
			"Analyze file (*.hdr *.img)\n"
			"Nrrd file (*.nrrd)\n"
			"VTK file (*.vtk *vti)\n"
			"BMP file (*.bmp)\n"
			"PNG file (*.png)\n"
			"JPG file (*.jpg *.jpeg)";

	std::string file_path = RecentPlaces::getSaveFileName(this, "Save As", QString::null, filter).toStdString();
	auto img_selection = static_cast<ImageWriter::eImageSelection>(img_selection_group->checkedId());
	auto slice_selection = static_cast<ImageWriter::eSliceSelection>(slice_selection_group->checkedId());

	ImageWriter w(true);
	w.writeVolume(file_path, handler3D, img_selection, slice_selection);

	close();
}

LoaderDicom::LoaderDicom(SlicesHandler* hand3D, QStringList* lname,
		bool breload, QWidget* parent, const char* name,
		Qt::WindowFlags wFlags)
		: QDialog(parent, name, TRUE, wFlags), handler3D(hand3D), reload(breload),
			lnames(lname)
{
	vbox1 = new Q3VBox(this);
	hbox1 = new Q3HBox(vbox1);
	cb_subsect = new QCheckBox(QString("Subsection "), hbox1);
	cb_subsect->setChecked(false);
	cb_subsect->show();
	hbox2 = new Q3HBox(hbox1);

	vbox2 = new Q3VBox(hbox2);
	vbox3 = new Q3VBox(hbox2);
	xoffs = new QLabel(QString("x-Offset: "), vbox2);
	xoffset = new QSpinBox(0, 2000, 1, vbox3);
	xoffset->setValue(0);
	xoffset->show();
	yoffs = new QLabel(QString("y-Offset: "), vbox2);
	yoffset = new QSpinBox(0, 2000, 1, vbox3);
	yoffset->show();
	xoffset->setValue(0);
	vbox2->show();
	vbox3->show();

	vbox4 = new Q3VBox(hbox2);
	vbox5 = new Q3VBox(hbox2);
	xl = new QLabel(QString("x-Length: "), vbox4);
	xlength = new QSpinBox(0, 2000, 1, vbox5);
	xlength->show();
	xlength->setValue(512);
	yl = new QLabel(QString("y-Length: "), vbox4);
	ylength = new QSpinBox(0, 2000, 1, vbox5);
	ylength->show();
	ylength->setValue(512);
	vbox4->show();
	vbox5->show();

	hbox2->show();
	hbox1->show();

	hbox3 = new Q3HBox(vbox1);
	cb_ct = new QCheckBox(QString("CT weight "), hbox3);
	cb_ct->setChecked(false);
	cb_ct->show();
	vbox6 = new Q3VBox(hbox3);
	hbox4 = new Q3HBox(vbox6);
	bg_weight = new QButtonGroup(this);
	//	bg_weight->hide();
	rb_bone = new QRadioButton(QString("Bone"), hbox4);
	rb_muscle = new QRadioButton(QString("Muscle"), hbox4);
	bg_weight->insert(rb_bone);
	bg_weight->insert(rb_muscle);
	rb_muscle->setChecked(TRUE);
	rb_muscle->show();
	rb_bone->show();
	hbox4->show();
	cb_crop = new QCheckBox(QString("crop "), vbox6);
	cb_crop->setChecked(true);
	cb_crop->show();
	vbox6->show();
	hbox3->show();
	//	hbox3->hide();

	if (!lnames->empty())
	{
		std::vector<const char*> vnames;
		for (auto it = lnames->begin(); it != lnames->end(); it++)
		{
			vnames.push_back((*it).ascii());
		}
		dicomseriesnr.clear();
		handler3D->GetDICOMseriesnr(&vnames, &dicomseriesnr, &dicomseriesnrlist);
		if (dicomseriesnr.size() > 1)
		{
			hbox6 = new Q3HBox(vbox1);

			lb_title = new QLabel(QString("Series-Nr.: "), hbox6);
			seriesnrselection = new QComboBox(hbox6);
			for (unsigned i = 0; i < (unsigned)dicomseriesnr.size(); i++)
			{
				QString str;
				seriesnrselection->insertItem(
						str = str.setNum((int)dicomseriesnr.at(i)));
			}
			seriesnrselection->setCurrentItem(0);
			hbox6->show();
		}
	}

	hbox5 = new Q3HBox(vbox1);
	loadFile = new QPushButton("Open", hbox5);
	cancelBut = new QPushButton("Cancel", hbox5);
	hbox5->show();
	vbox1->show();

	/*	vbox1->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	vbox2->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	hbox1->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	hbox2->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	hbox3->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	hbox4->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	hbox5->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));*/
	setSizePolicy(QSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed));
	vbox1->setFixedSize(vbox1->sizeHint());

	subsect_toggled();
	ct_toggled();

	QObject::connect(loadFile, SIGNAL(clicked()), this, SLOT(load_pushed()));
	QObject::connect(cancelBut, SIGNAL(clicked()), this, SLOT(close()));
	QObject::connect(cb_subsect, SIGNAL(clicked()), this,
			SLOT(subsect_toggled()));
	QObject::connect(cb_ct, SIGNAL(clicked()), this, SLOT(ct_toggled()));

	return;
}

LoaderDicom::~LoaderDicom()
{
	delete vbox1;
	delete bg_weight;
}

void LoaderDicom::subsect_toggled()
{
	if (cb_subsect->isChecked())
	{
		hbox2->show();
	}
	else
	{
		hbox2->hide();
	}
}

void LoaderDicom::ct_toggled()
{
	if (cb_ct->isChecked())
	{
		vbox6->show();
	}
	else
	{
		vbox6->hide();
	}
}

void LoaderDicom::load_pushed()
{
	if (!lnames->empty())
	{
		std::vector<const char*> vnames;
		if (dicomseriesnr.size() > 1)
		{
			unsigned pos = 0;
			for (const auto& name : *lnames)
			{
				if (dicomseriesnrlist[pos++] == dicomseriesnr[seriesnrselection->currentItem()])
				{
					vnames.push_back(name.ascii());
				}
			}
		}
		else
		{
			for (const auto& name : *lnames)
			{
				vnames.push_back(name.ascii());
			}
		}

		if (cb_subsect->isChecked())
		{
			Point p;
			p.px = xoffset->value();
			p.py = yoffset->value();
			if (reload)
				handler3D->ReloadDICOM(vnames, p);
			else
				handler3D->LoadDICOM(vnames, p, xlength->value(), ylength->value());
		}
		else
		{
			if (reload)
				handler3D->ReloadDICOM(vnames);
			else
				handler3D->LoadDICOM(vnames);
		}

		if (cb_ct->isChecked())
		{
			Pair p;
			if (rb_muscle->isChecked())
			{
				p.high = 1190;
				p.low = 890;
			}
			else if (rb_bone->isChecked())
			{
				handler3D->get_range(&p);
			}
			handler3D->scale_colors(p);
			if (cb_crop->isChecked())
			{
				handler3D->crop_colors();
			}
			handler3D->work2bmpall();
		}

		close();
	}
	else
	{
		close();
	}
}

LoaderRaw::LoaderRaw(SlicesHandler* hand3D, QWidget* parent, const char* name,
		Qt::WindowFlags wFlags)
		: QDialog(parent, name, TRUE, wFlags), handler3D(hand3D)
{
	vbox1 = new Q3VBox(this);
	hbox1 = new Q3HBox(vbox1);
	fileName = new QLabel(QString("File Name: "), hbox1);
	nameEdit = new QLineEdit(hbox1);
	nameEdit->show();
	selectFile = new QPushButton("Select", hbox1);
	selectFile->show();
	hbox1->show();
	hbox6 = new Q3HBox(vbox1);
	xl1 = new QLabel(QString("Total x-Length: "), hbox6);
	xlength1 = new QSpinBox(0, 9999, 1, hbox6);
	xlength1->show();
	xlength1->setValue(512);
	yl1 = new QLabel(QString("Total y-Length: "), hbox6);
	ylength1 = new QSpinBox(0, 9999, 1, hbox6);
	ylength1->show();
	ylength1->setValue(512);
	/*	nrslice = new QLabel(QString("Slice Nr.: "),hbox6);
	slicenrbox=new QSpinBox(0,200,1,hbox6);
	slicenrbox->show();
	slicenrbox->setValue(0);*/
	hbox6->show();
	hbox8 = new Q3HBox(vbox1);
	nrslice = new QLabel(QString("Start Nr.: "), hbox8);
	slicenrbox = new QSpinBox(0, 9999, 1, hbox8);
	slicenrbox->show();
	slicenrbox->setValue(0);
	lb_nrslices = new QLabel("#Slices: ", hbox8);
	sb_nrslices = new QSpinBox(1, 9999, 1, hbox8);
	sb_nrslices->show();
	sb_nrslices->setValue(10);
	hbox8->show();
	hbox2 = new Q3HBox(vbox1);
	subsect = new QCheckBox(QString("Subsection "), hbox2);
	subsect->setChecked(false);
	subsect->show();
	vbox2 = new Q3VBox(hbox2);
	hbox3 = new Q3HBox(vbox2);
	xoffs = new QLabel(QString("x-Offset: "), hbox3);
	xoffset = new QSpinBox(0, 2000, 1, hbox3);
	xoffset->setValue(0);
	xoffset->show();
	yoffs = new QLabel(QString("y-Offset: "), hbox3);
	yoffset = new QSpinBox(0, 2000, 1, hbox3);
	yoffset->show();
	xoffset->setValue(0);
	hbox3->show();
	hbox4 = new Q3HBox(vbox2);
	xl = new QLabel(QString("x-Length: "), hbox4);
	xlength = new QSpinBox(0, 2000, 1, hbox4);
	xlength->show();
	xlength->setValue(256);
	yl = new QLabel(QString("y-Length: "), hbox4);
	ylength = new QSpinBox(0, 2000, 1, hbox4);
	ylength->show();
	ylength->setValue(256);
	hbox4->show();
	vbox2->show();
	hbox2->show();
	hbox7 = new Q3HBox(vbox1);
	bitselect = new QButtonGroup(this);
	//	bitselect->hide();
	/*	bit8=new QRadioButton(QString("8-bit"),bitselect);
	bit16=new QRadioButton(QString("16-bit"),bitselect);*/
	bit8 = new QRadioButton(QString("8-bit"), hbox7);
	bit16 = new QRadioButton(QString("16-bit"), hbox7);
	bitselect->insert(bit8);
	bitselect->insert(bit16);
	bit16->show();
	bit8->setChecked(TRUE);
	bit8->show();
	hbox7->show();
	/*	bitselect->insert(bit8);
	bitselect->insert(bit16);*/

	hbox5 = new Q3HBox(vbox1);
	loadFile = new QPushButton("Open", hbox5);
	cancelBut = new QPushButton("Cancel", hbox5);
	hbox5->show();

	/*	vbox1->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	vbox2->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	hbox1->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	hbox2->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	hbox3->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	hbox4->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	hbox5->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));*/
	vbox1->show();
	setSizePolicy(QSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed));

	vbox1->setFixedSize(vbox1->sizeHint());

	QObject::connect(selectFile, SIGNAL(clicked()), this,
			SLOT(select_pushed()));
	QObject::connect(loadFile, SIGNAL(clicked()), this, SLOT(load_pushed()));
	QObject::connect(cancelBut, SIGNAL(clicked()), this, SLOT(close()));
	//	QObject::connect(subsect,SIGNAL(toggled(bool on)),this,SLOT(subsect_toggled(bool on)));
	QObject::connect(subsect, SIGNAL(clicked()), this, SLOT(subsect_toggled()));
	//	QObject::connect(subsect,SIGNAL(toggled(bool on)),vbox2,SLOT(setShown(bool on)));

	subsect_toggled();
}

LoaderRaw::~LoaderRaw()
{
	delete vbox1;
	delete bitselect;
}

QString LoaderRaw::GetFileName() const { return nameEdit->text(); }

std::array<unsigned int, 2> LoaderRaw::getDimensions() const
{
	return {
			static_cast<unsigned>(xlength1->value()),
			static_cast<unsigned>(ylength1->value())};
}

std::array<unsigned int, 3> LoaderRaw::getSubregionStart() const
{
	return {
			static_cast<unsigned>(xoffset->value()),
			static_cast<unsigned>(yoffset->value()),
			static_cast<unsigned>(slicenrbox->value())};
}

std::array<unsigned int, 3> LoaderRaw::getSubregionSize() const
{
	return {
			static_cast<unsigned>(subsect->isChecked() ? xlength->value() : xlength1->value()),
			static_cast<unsigned>(subsect->isChecked() ? ylength->value() : ylength1->value()),
			static_cast<unsigned>(sb_nrslices->value())};
}

int LoaderRaw::getBits() const
{
	return (bit8->isChecked() ? 8 : 16);
}

void LoaderRaw::subsect_toggled()
{
	bool isset = subsect->isChecked();
	;
	if (isset)
	{
		vbox2->show();
		//		nameEdit->setText(QString("ABC"));
	}
	else
	{
		vbox2->hide();
		//		nameEdit->setText(QString("DEF"));
	}

	//	vbox1->setFixedSize(vbox1->sizeHint());
}

void LoaderRaw::load_pushed()
{
	unsigned bitdepth;
	if (bit8->isChecked())
		bitdepth = 8;
	else
		bitdepth = 16;
	if (!(nameEdit->text()).isEmpty())
	{
		if (skip_reading)
		{
			// do nothing
		}
		else if (subsect->isOn())
		{
			Point p;
			p.px = xoffset->value();
			p.py = yoffset->value();
			handler3D->ReadRaw(
					nameEdit->text().ascii(), (unsigned short)xlength1->value(),
					(unsigned short)ylength1->value(), (unsigned)bitdepth,
					(unsigned short)slicenrbox->value(),
					(unsigned short)sb_nrslices->value(), p,
					(unsigned short)xlength->value(),
					(unsigned short)ylength->value());
		}
		else
		{
			handler3D->ReadRaw(
					nameEdit->text().ascii(), (unsigned short)xlength1->value(),
					(unsigned short)ylength1->value(), (unsigned)bitdepth,
					(unsigned short)slicenrbox->value(),
					(unsigned short)sb_nrslices->value());
		}
		close();
		return;
	}
	else
	{
		return;
	}
}

void LoaderRaw::select_pushed()
{
	QString loadfilename = RecentPlaces::getOpenFileName(this, "Open file", QString::null, QString::null);
	nameEdit->setText(loadfilename);
}

ReloaderRaw::ReloaderRaw(SlicesHandler* hand3D, QWidget* parent,
		const char* name, Qt::WindowFlags wFlags)
		: QDialog(parent, name, TRUE, wFlags)
{
	handler3D = hand3D;

	vbox1 = new Q3VBox(this);
	hbox1 = new Q3HBox(vbox1);
	fileName = new QLabel(QString("File Name: "), hbox1);
	nameEdit = new QLineEdit(hbox1);
	nameEdit->show();
	selectFile = new QPushButton("Select", hbox1);
	selectFile->show();
	hbox1->show();
	hbox2 = new Q3HBox(vbox1);
	bitselect = new QButtonGroup(this);
	//	bitselect->hide();
	bit8 = new QRadioButton(QString("8-bit"), hbox2);
	bit16 = new QRadioButton(QString("16-bit"), hbox2);
	bitselect->insert(bit8);
	bitselect->insert(bit16);
	bit16->show();
	bit8->setChecked(TRUE);
	bit8->show();
	nrslice = new QLabel(QString("Slice Nr.: "), hbox2);
	slicenrbox = new QSpinBox(0, 200, 1, hbox2);
	slicenrbox->show();
	slicenrbox->setValue(0);
	hbox2->show();
	hbox5 = new Q3HBox(vbox1);
	subsect = new QCheckBox(QString("Subsection "), hbox5);
	subsect->setChecked(false);
	subsect->show();
	vbox2 = new Q3VBox(hbox5);
	hbox4 = new Q3HBox(vbox2);
	xl1 = new QLabel(QString("Total x-Length: "), hbox4);
	xlength1 = new QSpinBox(0, 2000, 1, hbox4);
	xlength1->show();
	xlength1->setValue(512);
	yl1 = new QLabel(QString("Total  y-Length: "), hbox4);
	ylength1 = new QSpinBox(0, 2000, 1, hbox4);
	ylength1->show();
	ylength1->setValue(512);
	hbox4->show();
	hbox3 = new Q3HBox(vbox2);
	xoffs = new QLabel(QString("x-Offset: "), hbox3);
	xoffset = new QSpinBox(0, 2000, 1, hbox3);
	xoffset->setValue(0);
	xoffset->show();
	yoffs = new QLabel(QString("y-Offset: "), hbox3);
	yoffset = new QSpinBox(0, 2000, 1, hbox3);
	yoffset->show();
	xoffset->setValue(0);
	hbox3->show();
	vbox2->show();
	hbox5->show();

	/*	bitselect->insert(bit8);
	bitselect->insert(bit16);*/

	/*	hbox7=new QHBox(vbox1);
	lb_startnr = new QLabel(QString("Start Nr.: "),hbox7);
	sb_startnr=new QSpinBox(0,2000,1,hbox7);
	sb_startnr->setValue(0);
	sb_startnr->show();
	hbox7->show();*/

	hbox6 = new Q3HBox(vbox1);
	loadFile = new QPushButton("Open", hbox6);
	cancelBut = new QPushButton("Cancel", hbox6);
	hbox6->show();

	/*	vbox1->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	vbox2->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	hbox1->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	hbox2->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	hbox3->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	hbox4->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	hbox5->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));*/
	vbox1->show();
	setSizePolicy(QSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed));

	vbox1->setFixedSize(vbox1->sizeHint());

	subsect_toggled();

	QObject::connect(selectFile, SIGNAL(clicked()), this,
			SLOT(select_pushed()));
	QObject::connect(loadFile, SIGNAL(clicked()), this, SLOT(load_pushed()));
	QObject::connect(cancelBut, SIGNAL(clicked()), this, SLOT(close()));
	QObject::connect(subsect, SIGNAL(clicked()), this, SLOT(subsect_toggled()));
}

ReloaderRaw::~ReloaderRaw()
{
	delete vbox1;
	delete bitselect;
}

void ReloaderRaw::subsect_toggled()
{
	bool isset = subsect->isChecked();
	;
	if (isset)
	{
		vbox2->show();
		//		nameEdit->setText(QString("ABC"));
	}
	else
	{
		vbox2->hide();
		//		nameEdit->setText(QString("DEF"));
	}

	//	vbox1->setFixedSize(vbox1->sizeHint());
}

void ReloaderRaw::load_pushed()
{
	unsigned bitdepth;
	if (bit8->isChecked())
		bitdepth = 8;
	else
		bitdepth = 16;

	if (!(nameEdit->text()).isEmpty())
	{
		if (subsect->isChecked())
		{
			Point p;
			p.px = xoffset->value();
			p.py = yoffset->value();
			handler3D->ReloadRaw(nameEdit->text().ascii(), xlength1->value(),
					ylength1->value(), bitdepth,
					slicenrbox->value(), p);
		}
		else
		{
			handler3D->ReloadRaw(nameEdit->text().ascii(), bitdepth,
					slicenrbox->value());
		}
		close();
		return;
	}
	else
	{
		return;
	}
}

void ReloaderRaw::select_pushed()
{
	// null filter?
	QString loadfilename = RecentPlaces::getOpenFileName(this, QString::null, QString::null, QString::null);
	nameEdit->setText(loadfilename);
}

NewImg::NewImg(SlicesHandler* hand3D, QWidget* parent, const char* name,
		Qt::WindowFlags wFlags)
		: QDialog(parent, name, TRUE, wFlags)
{
	handler3D = hand3D;

	vbox1 = new Q3VBox(this);
	hbox2 = new Q3HBox(vbox1);
	xl = new QLabel(QString("Total x-Length: "), hbox2);
	xlength = new QSpinBox(1, 2000, 1, hbox2);
	xlength->show();
	xlength->setValue(512);
	yl = new QLabel(QString("Total  y-Length: "), hbox2);
	ylength = new QSpinBox(1, 2000, 1, hbox2);
	ylength->show();
	ylength->setValue(512);
	hbox2->show();
	hbox1 = new Q3HBox(vbox1);
	lb_nrslices = new QLabel(QString("# Slices: "), hbox1);
	sb_nrslices = new QSpinBox(1, 2000, 1, hbox1);
	sb_nrslices->show();
	sb_nrslices->setValue(10);
	hbox1->show();
	hbox3 = new Q3HBox(vbox1);
	newFile = new QPushButton("New", hbox3);
	cancelBut = new QPushButton("Cancel", hbox3);
	hbox3->show();

	vbox1->show();
	setSizePolicy(QSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed));

	vbox1->setFixedSize(vbox1->sizeHint());

	QObject::connect(newFile, SIGNAL(clicked()), this, SLOT(new_pushed()));
	QObject::connect(cancelBut, SIGNAL(clicked()), this, SLOT(on_close()));

	newPressed = false;
}

NewImg::~NewImg() { delete vbox1; }

bool NewImg::new_pressed() const { return newPressed; }

void NewImg::new_pushed()
{
	handler3D->UpdateColorLookupTable(nullptr);

	handler3D->newbmp((unsigned short)xlength->value(),
			(unsigned short)ylength->value(),
			(unsigned short)sb_nrslices->value());
	newPressed = true;
	close();
}

void NewImg::on_close()
{
	newPressed = false;
	close();
}

LoaderColorImages::LoaderColorImages(SlicesHandler* hand3D, eImageType typ, std::vector<const char*> filenames,
		QWidget* parent, const char* name, Qt::WindowFlags wFlags)
		: QDialog(parent, name, TRUE, wFlags), handler3D(hand3D), type(typ), m_filenames(filenames)
{
	map_to_lut = new QCheckBox(QString("Map colors to lookup table"));
	map_to_lut->setChecked(true);
	if (typ == LoaderColorImages::kTIF)
	{
		map_to_lut->setEnabled(false);
	}

	subsect = new QCheckBox(QString("Subsection"));
	subsect->setChecked(false);

	auto xoffs = new QLabel(QString("x-Offset: "));
	xoffset = new QSpinBox(0, 2000, 1, nullptr);
	xoffset->setValue(0);

	auto yoffs = new QLabel(QString("y-Offset: "));
	yoffset = new QSpinBox(0, 2000, 1, nullptr);
	xoffset->setValue(0);

	auto xl = new QLabel(QString("x-Length: "));
	xlength = new QSpinBox(0, 2000, 1, nullptr);
	xlength->setValue(256);

	auto yl = new QLabel(QString("y-Length: "));
	ylength = new QSpinBox(0, 2000, 1, nullptr);
	ylength->setValue(256);

	auto subsect_layout = new QGridLayout(2, 4);
	subsect_layout->addWidget(xoffs);
	subsect_layout->addWidget(xoffset);
	subsect_layout->addWidget(xl);
	subsect_layout->addWidget(xlength);
	subsect_layout->addWidget(yoffs);
	subsect_layout->addWidget(yoffset);
	subsect_layout->addWidget(yl);
	subsect_layout->addWidget(ylength);
	auto subsect_options = new QWidget;
	subsect_options->setLayout(subsect_layout);

	loadFile = new QPushButton("Open");
	cancelBut = new QPushButton("Cancel");
	auto button_layout = new QHBoxLayout;
	button_layout->addWidget(loadFile);
	button_layout->addWidget(cancelBut);
	auto button_row = new QWidget;
	button_row->setLayout(button_layout);

	auto top_layout = new QVBoxLayout;
	top_layout->addWidget(map_to_lut);
	top_layout->addWidget(subsect);
	top_layout->addWidget(subsect_options);
	top_layout->addWidget(button_row);
	setLayout(top_layout);
	setMinimumSize(150, 200);

	map_to_lut_toggled();

	QObject::connect(loadFile, SIGNAL(clicked()), this, SLOT(load_pushed()));
	QObject::connect(cancelBut, SIGNAL(clicked()), this, SLOT(close()));
	QObject::connect(map_to_lut, SIGNAL(clicked()), this, SLOT(map_to_lut_toggled()));
}

LoaderColorImages::~LoaderColorImages() {}

void LoaderColorImages::map_to_lut_toggled()
{
	// enable/disable
	subsect->setEnabled(!map_to_lut->isChecked());
}

void LoaderColorImages::load_pushed()
{
	if (map_to_lut->isChecked())
	{
		load_quantize();
	}
	else
	{
		load_mixer();
	}
}

void LoaderColorImages::load_quantize()
{
	QString initialDir = QString::null;

	auto lut_path = boost::dll::program_location().parent_path() / fs::path("lut");
	if (fs::exists(lut_path))
	{
		fs::directory_iterator dir_itr(lut_path);
		fs::directory_iterator end_iter;
		for (; dir_itr != end_iter; ++dir_itr)
		{
			fs::path lut_file(dir_itr->path());
			if (algo::iends_with(lut_file.string(), ".lut"))
			{
				initialDir = QString::fromStdString(lut_file.parent_path().string());
				break;
			}
		}
	}

	QString filename = RecentPlaces::getOpenFileName(this, "Open Lookup Table", initialDir,
			"iSEG Color Lookup Table (*.lut *.h5)\nAll (*.*)");
	if (!filename.isEmpty())
	{
		XdmfImageReader reader;
		reader.SetFileName(filename.toStdString().c_str());
		std::shared_ptr<ColorLookupTable> lut;
		{
			ScopedTimer t("Load LUT");
			lut = reader.ReadColorLookup();
		}
		if (lut)
		{
			const auto N = lut->NumberOfColors();

			using color_t = std::array<unsigned char, 3>;
			using color_vector_t = std::vector<color_t>;

			color_vector_t points(N);
			for (size_t i = 0; i < N; ++i)
			{
				lut->GetColor(i, points[i].data());
			}

			using distance_t = float;
			using my_kd_tree_t = KDTreeVectorOfVectorsAdaptor<color_vector_t, distance_t>;
			my_kd_tree_t tree(3, points, 10 /* max leaf */);
			{
				ScopedTimer t("Build kd-tree for colors");
				tree.index->buildIndex();
			}

			unsigned w, h;
			if (ImageReader::getInfo2D(m_filenames[0], w, h))
			{
				const auto map_colors = [&tree](unsigned char r, unsigned char g, unsigned char b) -> float {
					size_t id;
					distance_t sqr_dist;
					std::array<distance_t, 3> query_pt = {
							static_cast<distance_t>(r),
							static_cast<distance_t>(g),
							static_cast<distance_t>(b)};
					tree.query(&query_pt[0], 1, &id, &sqr_dist);
					return static_cast<float>(id);
				};

				auto load = [&, this](float** slices) {
					ScopedTimer t("Load and map image stack");
					ImageReader::getImageStack(m_filenames, slices, w, h, map_colors);
				};

				handler3D->newbmp(w, h, static_cast<unsigned short>(m_filenames.size()), load);
				handler3D->UpdateColorLookupTable(lut);
			}
		}
		else
		{
			QMessageBox::warning(this, "iSeg",
					"ERROR: occurred while reading color lookup table\n", QMessageBox::Ok | QMessageBox::Default);
		}
	}

	close();
}

void LoaderColorImages::load_mixer()
{
	if ((type == eImageType::kBMP && bmphandler::CheckBMPDepth(m_filenames[0]) > 8) ||
			(type == eImageType::kPNG && bmphandler::CheckPNGDepth(m_filenames[0]) > 8))
	{
		ChannelMixer channelMixer(m_filenames, nullptr);
		channelMixer.move(QCursor::pos());
		if (!channelMixer.exec())
		{
			close();
			return;
		}

		int redFactor = channelMixer.GetRedFactor();
		int greenFactor = channelMixer.GetGreenFactor();
		int blueFactor = channelMixer.GetBlueFactor();
		handler3D->set_rgb_factors(redFactor, greenFactor, blueFactor);
	}
	else
	{
		handler3D->set_rgb_factors(33, 33, 33);
	}

	if (subsect->isChecked())
	{
		Point p;
		p.px = xoffset->value();
		p.py = yoffset->value();

		switch (type)
		{
		case eImageType::kPNG:
			handler3D->LoadPng(m_filenames, p, xlength->value(), ylength->value());
			break;
		case eImageType::kBMP:
			handler3D->LoadDIBitmap(m_filenames, p, xlength->value(), ylength->value());
			break;
		case eImageType::kJPG:
			handler3D->LoadDIJpg(m_filenames, p, xlength->value(), ylength->value());
			break;
		}
	}
	else
	{
		switch (type)
		{
		case eImageType::kPNG:
			handler3D->LoadPng(m_filenames);
			break;
		case eImageType::kBMP:
			handler3D->LoadDIBitmap(m_filenames);
			break;
		case eImageType::kJPG:
			handler3D->LoadDIJpg(m_filenames);
			break;
		}
	}
	close();
}

ClickableLabel::ClickableLabel(QWidget* parent, Qt::WindowFlags f)
		: QLabel(parent, f)
{
	centerX = width() / 2;
	centerY = height() / 2;
	squareWidth = 24;
	squareHeight = 24;
}

ClickableLabel::ClickableLabel(const QString& text, QWidget* parent,
		Qt::WindowFlags f)
		: QLabel(text, parent, f)
{
}

void ClickableLabel::SetSquareWidth(int width) { squareWidth = width; }

void ClickableLabel::SetSquareHeight(int height) { squareHeight = height; }

void ClickableLabel::SetCenter(QPoint newCenter)
{
	centerX = newCenter.x();
	centerY = newCenter.y();
	emit newCenterPreview(QPoint(centerX, centerY));
}

void ClickableLabel::mousePressEvent(QMouseEvent* ev)
{
	centerX = ev->pos().x();
	centerY = ev->pos().y();
	emit newCenterPreview(QPoint(centerX, centerY));
}

void ClickableLabel::paintEvent(QPaintEvent* e)
{
	QLabel::paintEvent(e);

	QPainter painter(this);

	QPen paintpen(Qt::yellow);
	paintpen.setWidth(1);
	painter.setPen(paintpen);

	QPainterPath square;
	square.addRect(centerX - squareHeight / 2, centerY - squareHeight / 2,
			squareWidth, squareHeight);
	painter.drawPath(square);
}

ChannelMixer::ChannelMixer(std::vector<const char*> filenames, QWidget* parent,
		const char* name, Qt::WindowFlags wFlags)
		: QDialog(parent, name, TRUE, wFlags), m_filenames(filenames)
{
	previewCenter.setX(0);
	previewCenter.setY(0);
	QString fileName = QString::fromUtf8(m_filenames[0]);
	if (!fileName.isEmpty())
	{
		sourceImage = QImage(fileName);
		if (sourceImage.isNull())
		{
			QMessageBox::information(this, tr("Image Viewer"),
					tr("Cannot load %1.").arg(fileName));
			return;
		}
		previewCenter.setX(sourceImage.width());
		previewCenter.setY(sourceImage.height());
	}

	redFactorPV = 30;
	greenFactorPV = 59;
	blueFactorPV = 11;

	redFactor = 30;
	greenFactor = 59;
	blueFactor = 11;

	scaleX = 400;
	scaleY = 500;

	vboxMain = new Q3VBox(this);

	hboxImageAndControl = new Q3HBox(vboxMain);

	QSize standardBoxSize;
	standardBoxSize.setWidth(scaleX);
	standardBoxSize.setHeight(scaleY);

	hboxImageSource = new Q3HBox(hboxImageAndControl);
	hboxImageSource->setFixedSize(standardBoxSize);
	hboxImageSource->show();
	imageSourceLabel = new ClickableLabel(hboxImageSource);
	imageSourceLabel->setFixedSize(standardBoxSize);
	imageSourceLabel->SetSquareWidth(25);
	imageSourceLabel->SetSquareHeight(25);
	imageSourceLabel->setAlignment(Qt::AlignCenter);

	hboxImage = new Q3HBox(hboxImageAndControl);
	hboxImage->setFixedSize(standardBoxSize);
	hboxImage->show();
	imageLabel = new QLabel(hboxImage);
	imageLabel->setFixedSize(standardBoxSize);

	hboxControl = new Q3VBox(hboxImageAndControl);

	QSize controlSize;
	controlSize.setHeight(scaleY);
	controlSize.setWidth(scaleX / 2);
	hboxControl->setFixedSize(controlSize);

	hboxChannelOptions = new Q3VBox(hboxControl);

	vboxRed = new Q3HBox(hboxChannelOptions);
	vboxGreen = new Q3HBox(hboxChannelOptions);
	vboxBlue = new Q3HBox(hboxChannelOptions);
	labelPreviewAlgorithm = new QLabel(hboxChannelOptions);
	vboxSlice = new Q3HBox(hboxChannelOptions);
	hboxButtons = new Q3HBox(hboxChannelOptions);

	labelRed = new QLabel(vboxRed);
	labelRed->setText("Red");
	labelRed->setFixedWidth(40);
	sliderRed = new QSlider(Qt::Horizontal, vboxRed);
	sliderRed->setMinValue(0);
	sliderRed->setMaxValue(100);
	sliderRed->setValue(redFactor);
	sliderRed->setFixedWidth(80);
	labelRedValue = new QLineEdit(vboxRed);
	labelRedValue->setText(QString::number(sliderRed->value()));
	labelRedValue->setFixedWidth(30);
	QLabel* labelPureRed = new QLabel(vboxRed);
	labelPureRed->setText(" Pure");
	labelPureRed->setFixedWidth(30);
	buttonRed = new QRadioButton(vboxRed);
	buttonRed->setChecked(false);

	labelGreen = new QLabel(vboxGreen);
	labelGreen->setText("Green");
	labelGreen->setFixedWidth(40);
	sliderGreen = new QSlider(Qt::Horizontal, vboxGreen);
	sliderGreen->setMinValue(0);
	sliderGreen->setMaxValue(100);
	sliderGreen->setValue(greenFactor);
	sliderGreen->setFixedWidth(80);
	labelGreenValue = new QLineEdit(vboxGreen);
	labelGreenValue->setText(QString::number(sliderGreen->value()));
	labelGreenValue->setFixedWidth(30);
	QLabel* labelPureGreen = new QLabel(vboxGreen);
	labelPureGreen->setText(" Pure");
	labelPureGreen->setFixedWidth(30);
	buttonGreen = new QRadioButton(vboxGreen);
	buttonGreen->setChecked(false);

	labelBlue = new QLabel(vboxBlue);
	labelBlue->setText("Blue");
	labelBlue->setFixedWidth(40);
	sliderBlue = new QSlider(Qt::Horizontal, vboxBlue);
	sliderBlue->setMinValue(0);
	sliderBlue->setMaxValue(100);
	sliderBlue->setValue(blueFactor);
	sliderBlue->setFixedWidth(80);
	labelBlueValue = new QLineEdit(vboxBlue);
	labelBlueValue->setText(QString::number(sliderBlue->value()));
	labelBlueValue->setFixedWidth(30);
	QLabel* labelPureBlue = new QLabel(vboxBlue);
	labelPureBlue->setText(" Pure");
	labelPureBlue->setFixedWidth(30);
	buttonBlue = new QRadioButton(vboxBlue);
	buttonBlue->setChecked(false);

	labelSliceValue = new QLabel(vboxSlice);
	labelSliceValue->setText("Slice");
	labelSliceValue->setFixedWidth(40);
	spinSlice = new QSpinBox(vboxSlice);
	spinSlice->setMinimum(0);
	spinSlice->setMaximum(static_cast<int>(filenames.size()) - 1);
	spinSlice->setValue(0);
	selectedSlice = spinSlice->value();

	loadFile = new QPushButton("Open", hboxButtons);
	cancelBut = new QPushButton("Cancel", hboxButtons);

	hboxControl->show();
	hboxButtons->show();
	vboxMain->show();

	setSizePolicy(QSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed));
	vboxMain->setFixedSize(vboxMain->sizeHint());

	QObject::connect(sliderRed, SIGNAL(valueChanged(int)), this,
			SLOT(sliderRedValueChanged(int)));
	QObject::connect(sliderGreen, SIGNAL(valueChanged(int)), this,
			SLOT(sliderGreenValueChanged(int)));
	QObject::connect(sliderBlue, SIGNAL(valueChanged(int)), this,
			SLOT(sliderBlueValueChanged(int)));

	QObject::connect(labelRedValue, SIGNAL(textEdited(QString)), this,
			SLOT(labelRedValueChanged(QString)));
	QObject::connect(labelGreenValue, SIGNAL(textEdited(QString)), this,
			SLOT(labelGreenValueChanged(QString)));
	QObject::connect(labelBlueValue, SIGNAL(textEdited(QString)), this,
			SLOT(labelBlueValueChanged(QString)));

	QObject::connect(buttonRed, SIGNAL(toggled(bool)), this,
			SLOT(buttonRedPushed(bool)));
	QObject::connect(buttonGreen, SIGNAL(toggled(bool)), this,
			SLOT(buttonGreenPushed(bool)));
	QObject::connect(buttonBlue, SIGNAL(toggled(bool)), this,
			SLOT(buttonBluePushed(bool)));

	QObject::connect(spinSlice, SIGNAL(valueChanged(int)), this,
			SLOT(sliceValueChanged(int)));

	QObject::connect(loadFile, SIGNAL(clicked()), this, SLOT(load_pushed()));
	QObject::connect(cancelBut, SIGNAL(clicked()), this, SLOT(close()));

	QObject::connect(imageSourceLabel, SIGNAL(newCenterPreview(QPoint)), this,
			SLOT(NewCenterPreview(QPoint)));

	firstTime = true;

	RefreshSourceImage();
	ChangePreview();
}

ChannelMixer::~ChannelMixer() { delete vboxMain; }

void ChannelMixer::sliderRedValueChanged(int value)
{
	redFactor = value;
	labelRedValue->setText(QString::number(value));

	ChangePreview();
}

void ChannelMixer::sliderGreenValueChanged(int value)
{
	greenFactor = value;
	labelGreenValue->setText(QString::number(value));

	ChangePreview();
}

void ChannelMixer::sliderBlueValueChanged(int value)
{
	blueFactor = value;
	labelBlueValue->setText(QString::number(value));

	ChangePreview();
}

void ChannelMixer::labelRedValueChanged(QString text)
{
	redFactor = text.toInt();
	sliderRed->setValue(redFactor);

	ChangePreview();
}

void ChannelMixer::labelGreenValueChanged(QString text)
{
	greenFactor = text.toInt();
	sliderGreen->setValue(greenFactor);

	ChangePreview();
}

void ChannelMixer::labelBlueValueChanged(QString text)
{
	blueFactor = text.toInt();
	sliderBlue->setValue(blueFactor);

	ChangePreview();
}

void ChannelMixer::buttonRedPushed(bool checked)
{
	if (checked)
	{
		sliderRed->setValue(100);
		sliderGreen->setValue(0);
		sliderBlue->setValue(0);

		buttonGreen->setChecked(false);
		buttonBlue->setChecked(false);
	}
}

void ChannelMixer::buttonGreenPushed(bool checked)
{
	if (checked)
	{
		sliderRed->setValue(0);
		sliderGreen->setValue(100);
		sliderBlue->setValue(0);

		buttonRed->setChecked(false);
		buttonBlue->setChecked(false);
	}
}
void ChannelMixer::buttonBluePushed(bool checked)
{
	if (checked)
	{
		sliderRed->setValue(0);
		sliderGreen->setValue(0);
		sliderBlue->setValue(100);

		buttonRed->setChecked(false);
		buttonGreen->setChecked(false);
	}
}

void ChannelMixer::sliceValueChanged(int value)
{
	selectedSlice = value;

	RefreshSourceImage();
}

void ChannelMixer::NewCenterPreview(QPoint newCenter)
{
	double imageWidth = sourceImage.width();
	double imageHeight = sourceImage.height();

	bool fixedInHeight = true;
	double scaledFactor = 1;
	if (imageHeight / imageWidth < scaleY / scaleX)
		fixedInHeight = false;

	int correctionX = 0;
	int correctionY = 0;
	if (fixedInHeight)
	{
		scaledFactor = imageHeight / scaleY;
		correctionX = (imageWidth - scaleX * scaledFactor) / 2;
	}
	else
	{
		scaledFactor = imageWidth / scaleX;
		correctionY = (scaleY * scaledFactor - imageHeight) / 2;
	}

	previewCenter.setX(scaledFactor * newCenter.x() + correctionX);
	previewCenter.setY(imageHeight -
										 (scaledFactor * newCenter.y() - correctionY));

	RefreshSourceImage();
	//RefreshImage();
}

void ChannelMixer::ChangePreview()
{
	if ((redFactor != 0) + (greenFactor != 0) + (blueFactor != 0) > 1)
	{
		buttonRed->setChecked(false);
		buttonGreen->setChecked(false);
		buttonBlue->setChecked(false);
	}

	int totalFactor = redFactor + greenFactor + blueFactor;
	double normalize = totalFactor / 100.0;

	if (normalize == 0)
	{
		redFactorPV = 33;
		greenFactorPV = 33;
		blueFactorPV = 33;
	}
	else
	{
		redFactorPV = redFactor / normalize;
		greenFactorPV = greenFactor / normalize;
		blueFactorPV = blueFactor / normalize;
	}

	UpdateText();
	RefreshImage();
}

void ChannelMixer::RefreshSourceImage()
{
	QString fileName = QString::fromUtf8(m_filenames[selectedSlice]);
	QImage smallImage;
	if (!fileName.isEmpty())
	{
		sourceImage = QImage(fileName);
		if (sourceImage.isNull())
		{
			QMessageBox::information(this, tr("Image Viewer"),
					tr("Cannot load %1.").arg(fileName));
			return;
		}

		smallImage = sourceImage.scaled(scaleX, scaleY, Qt::KeepAspectRatio);

		imageSourceLabel->setPixmap(QPixmap::fromImage(smallImage));
		imageSourceLabel->update();
	}
	hboxImageSource->update();

	if (firstTime)
	{
		firstTime = false;

		double imageSourceWidth = sourceImage.width();
		double imageSourceHeight = sourceImage.height();

		previewCenter.setX(imageSourceWidth / 2);
		previewCenter.setY(imageSourceHeight / 2);

		double smallImageWidth = smallImage.width();
		double smallImageHeight = smallImage.height();

		bool fixedInHeight = true;
		double scaledFactor = 1;
		if (imageSourceHeight / imageSourceWidth < scaleY / scaleX)
			fixedInHeight = false;

		if (fixedInHeight)
			scaledFactor = imageSourceHeight / scaleY;
		else
			scaledFactor = imageSourceWidth / scaleX;

		int squareWidth;
		if (imageSourceWidth > 900 || imageSourceHeight > 900)
			squareWidth = 300;
		else
		{
			squareWidth = std::min(imageSourceWidth / 3, imageSourceHeight / 3);
		}

		widthPV = heightPV = squareWidth;

		imageSourceLabel->SetSquareWidth(squareWidth / scaledFactor);
		imageSourceLabel->SetSquareHeight(squareWidth / scaledFactor);

		int smallImageCenterX = smallImageWidth / 2;
		int smallImageCenterY = smallImageHeight / 2;
		int smallImageCenterSquareHalfSide = squareWidth / scaledFactor / 2;
		smallImageCenterSquareHalfSide = 0;
		imageSourceLabel->SetCenter(QPoint(smallImageCenterX, smallImageCenterY + smallImageCenterSquareHalfSide));
	}

	RefreshImage();
}

void ChannelMixer::RefreshImage()
{
	QString fileName = QString::fromUtf8(m_filenames[selectedSlice]);
	if (!fileName.isEmpty())
	{
		QImage image(fileName);
		if (image.isNull())
		{
			QMessageBox::information(this, tr("Image Viewer"),
					tr("Cannot load %1.").arg(fileName));
			return;
		}

		QImage converted = ConvertImageTo8BitBMP(image, widthPV, heightPV);
		imageLabel->clear();
		imageLabel->setPixmap(QPixmap::fromImage(
				converted.scaled(scaleX, scaleY, Qt::KeepAspectRatio)));
		imageLabel->update();
	}
	hboxImage->update();
}

QImage ChannelMixer::ConvertImageTo8BitBMP(QImage image, int width, int height)
{
	/* Convert RGB image to grayscale image */
	QImage convertedImage(width, height, QImage::Format::Format_Indexed8);

	QVector<QRgb> table(256);
	for (int h = 0; h < 256; ++h)
	{
		table[h] = qRgb(h, h, h);
	}
	convertedImage.setColorTable(table);

	int startX = previewCenter.x() - (width / 2);
	int startY = sourceImage.height() - (previewCenter.y() + (height / 2));

	QRect rect(startX, startY, width, height);
	QImage cropped = image.copy(rect);

	for (int j = 2; j < height - 2; j++)
	{
		for (int i = 2; i < width - 2; i++)
		{
			QRgb rgb = cropped.pixel(i, j);
			int grayValue = qRed(rgb) * (redFactorPV / 100.00) +
											qGreen(rgb) * (greenFactorPV / 100.00) +
											qBlue(rgb) * (blueFactorPV / 100.00);
			convertedImage.setPixel(i, j, grayValue);
		}
	}

	return convertedImage;
}

void ChannelMixer::UpdateText()
{
	labelPreviewAlgorithm->setText(
			"GrayScale = " + QString::number(redFactorPV) + "*R + " +
			QString::number(greenFactorPV) + "*G + " +
			QString::number(blueFactorPV) + "*B");
}

void ChannelMixer::cancel_toggled()
{
	redFactorPV = 30;
	greenFactorPV = 59;
	blueFactorPV = 11;

	vboxMain->hide();
}

int ChannelMixer::GetRedFactor() { return redFactorPV; }

int ChannelMixer::GetGreenFactor() { return greenFactorPV; }

int ChannelMixer::GetBlueFactor() { return blueFactorPV; }

void ChannelMixer::load_pushed()
{
	close();
}

ReloaderBmp2::ReloaderBmp2(SlicesHandler* hand3D, std::vector<const char*> filenames,
		QWidget* parent, const char* name,
		Qt::WindowFlags wFlags)
		: QDialog(parent, name, TRUE, wFlags)
{
	handler3D = hand3D;
	m_filenames = filenames;

	vbox1 = new Q3VBox(this);
	hbox2 = new Q3HBox(vbox1);
	subsect = new QCheckBox(QString("Subsection "), hbox2);
	subsect->setChecked(false);
	subsect->show();
	xoffs = new QLabel(QString("x-Offset: "), hbox2);
	xoffset = new QSpinBox(0, 2000, 1, hbox2);
	xoffset->setValue(0);
	xoffset->show();
	yoffs = new QLabel(QString("y-Offset: "), hbox2);
	yoffset = new QSpinBox(0, 2000, 1, hbox2);
	yoffset->show();
	xoffset->setValue(0);
	hbox2->show();
	hbox3 = new Q3HBox(vbox1);
	loadFile = new QPushButton("Open", hbox3);
	cancelBut = new QPushButton("Cancel", hbox3);
	hbox3->show();

	/*	vbox1->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	vbox2->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	hbox1->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	hbox2->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	hbox3->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	hbox4->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));
	hbox5->setSizePolicy(QSizePolicy(QSizePolicy::Fixed,QSizePolicy::Fixed));*/
	vbox1->show();
	setSizePolicy(QSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed));

	vbox1->setFixedSize(vbox1->sizeHint());

	subsect_toggled();

	QObject::connect(loadFile, SIGNAL(clicked()), this, SLOT(load_pushed()));
	QObject::connect(cancelBut, SIGNAL(clicked()), this, SLOT(close()));
	QObject::connect(subsect, SIGNAL(clicked()), this, SLOT(subsect_toggled()));
}

ReloaderBmp2::~ReloaderBmp2() { delete vbox1; }

void ReloaderBmp2::subsect_toggled()
{
	bool isset = subsect->isChecked();
	;
	if (isset)
	{
		xoffset->show();
		yoffs->show();
		yoffset->show();
		yoffs->show();
	}
	else
	{
		xoffset->hide();
		xoffs->hide();
		yoffset->hide();
		yoffs->hide();
	}
}

void ReloaderBmp2::load_pushed()
{
	if (subsect->isChecked())
	{
		Point p;
		p.px = xoffset->value();
		p.py = yoffset->value();
		handler3D->ReloadDIBitmap(m_filenames, p);
	}
	else
	{
		handler3D->ReloadDIBitmap(m_filenames);
	}
	close();
}

EditText::EditText(QWidget* parent, const char* name, Qt::WindowFlags wFlags)
		: QDialog(parent, name, TRUE, wFlags)
{
	vbox1 = new Q3VBox(this);

	hbox1 = new Q3HBox(vbox1);
	text_edit = new QLineEdit(hbox1);

	hbox2 = new Q3HBox(vbox1);
	saveBut = new QPushButton("Save", hbox2);
	cancelBut = new QPushButton("Cancel", hbox2);

	setSizePolicy(QSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed));

	vbox1->setFixedSize(200, 50);

	QObject::connect(saveBut, SIGNAL(clicked()), this, SLOT(accept()));
	QObject::connect(cancelBut, SIGNAL(clicked()), this, SLOT(reject()));
}

EditText::~EditText() { delete vbox1; }

void EditText::set_editable_text(QString editable_text)
{
	text_edit->setText(editable_text);
}

QString EditText::get_editable_text() { return text_edit->text(); }

SupportedMultiDatasetTypes::SupportedMultiDatasetTypes(QWidget* parent,
		const char* name,
		Qt::WindowFlags wFlags)
		: QDialog(parent, name, TRUE, wFlags)
{
	hboxoverall = new Q3HBoxLayout(this);
	vboxoverall = new Q3VBoxLayout(this);
	hboxoverall->addLayout(vboxoverall);

	// Dataset selection group box
	QGroupBox* radioGroupBox = new QGroupBox("Supported types");
	Q3VBoxLayout* radioLayout = new Q3VBoxLayout(this);
	for (int i = 0; i < supportedTypes::nrSupportedTypes; i++)
	{
		QString texted = ToQString(supportedTypes(i));
		QRadioButton* radioBut = new QRadioButton(texted);
		radioLayout->addWidget(radioBut);

		m_RadioButs.push_back(radioBut);
	}
	radioGroupBox->setLayout(radioLayout);
	vboxoverall->addWidget(radioGroupBox);

	QHBoxLayout* buttonsLayout = new QHBoxLayout();
	selectBut = new QPushButton("Select", this);
	cancelBut = new QPushButton("Cancel", this);
	buttonsLayout->addWidget(selectBut);
	buttonsLayout->addWidget(cancelBut);
	vboxoverall->addLayout(buttonsLayout);

	setSizePolicy(QSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed));

	QObject::connect(selectBut, SIGNAL(clicked()), this, SLOT(accept()));
	QObject::connect(cancelBut, SIGNAL(clicked()), this, SLOT(reject()));
}

SupportedMultiDatasetTypes::~SupportedMultiDatasetTypes()
{
	delete vboxoverall;
}

int SupportedMultiDatasetTypes::GetSelectedType()
{
	for (int i = 0; i < m_RadioButs.size(); i++)
	{
		if (m_RadioButs.at(i)->isChecked())
		{
			return i;
		}
	}
	return -1;
}

} // namespace iseg
