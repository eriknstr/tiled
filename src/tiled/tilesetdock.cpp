/*
 * tilesetdock.cpp
 * Copyright 2008-2010, Thorbjørn Lindeijer <thorbjorn@lindeijer.nl>
 * Copyright 2009, Edward Hutchins <eah1@yahoo.com>
 * Copyright 2012, Stefan Beller <stefanbeller@googlemail.com>
 *
 * This file is part of Tiled.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation; either version 2 of the License, or (at your option)
 * any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include "tilesetdock.h"

#include "addremovemapobject.h"
#include "addremovetiles.h"
#include "addremovetileset.h"
#include "containerhelpers.h"
#include "documentmanager.h"
#include "erasetiles.h"
#include "map.h"
#include "mapdocument.h"
#include "mapobject.h"
#include "objectgroup.h"
#include "preferences.h"
#include "terrain.h"
#include "tile.h"
#include "tilelayer.h"
#include "tilesetdocument.h"
#include "tilesetformat.h"
#include "tilesetmodel.h"
#include "tilesetview.h"
#include "tilesetmanager.h"
#include "tilestamp.h"
#include "tmxmapformat.h"
#include "utils.h"
#include "zoomable.h"

#include <QMimeData>
#include <QAction>
#include <QDropEvent>
#include <QComboBox>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QMenu>
#include <QMessageBox>
#include <QSignalMapper>
#include <QStackedWidget>
#include <QStylePainter>
#include <QToolBar>
#include <QToolButton>
#include <QUrl>
#include <QVBoxLayout>

#include <functional>

using namespace Tiled;
using namespace Tiled::Internal;

namespace {

/**
 * Used for exporting/importing tilesets.
 *
 * @warning Does not work for tilesets that are shared by multiple maps!
 */
/*
class SetTilesetFileName : public QUndoCommand
{
public:
    SetTilesetFileName(MapDocument *mapDocument,
                       Tileset *tileset,
                       const QString &fileName)
        : mMapDocument(mapDocument)
        , mTileset(tileset)
        , mFileName(fileName)
    {
        if (fileName.isEmpty())
            setText(QCoreApplication::translate("Undo Commands",
                                                "Import Tileset"));
        else
            setText(QCoreApplication::translate("Undo Commands",
                                                "Export Tileset"));
    }

    void undo() override { swap(); }
    void redo() override { swap(); }

private:
    void swap()
    {
        QString previousFileName = mTileset->fileName();
        mMapDocument->setTilesetFileName(mTileset, mFileName);
        mFileName = previousFileName;
    }

    MapDocument *mMapDocument;
    Tileset *mTileset;
    QString mFileName;
};
*/

class TilesetMenuButton : public QToolButton
{
public:
    TilesetMenuButton(QWidget *parent = nullptr)
        : QToolButton(parent)
    {
        setArrowType(Qt::DownArrow);
        setIconSize(QSize(16, 16));
        setPopupMode(QToolButton::InstantPopup);
        setAutoRaise(true);

        setSizePolicy(sizePolicy().horizontalPolicy(),
                      QSizePolicy::Ignored);
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QStylePainter p(this);
        QStyleOptionToolButton opt;
        initStyleOption(&opt);

        // Disable the menu arrow, since we already got a down arrow icon
        opt.features &= ~QStyleOptionToolButton::HasMenu;

        p.drawComplexControl(QStyle::CC_ToolButton, opt);
    }
};


/**
 * Qt excludes OS X when implementing mouse wheel for switching tabs. However,
 * we explicitly want this feature on the tileset tab bar as a possible means
 * of navigation.
 */
class WheelEnabledTabBar : public QTabBar
{
public:
    WheelEnabledTabBar(QWidget *parent = nullptr)
       : QTabBar(parent)
    {}

    void wheelEvent(QWheelEvent *event) override
    {
        int index = currentIndex();
        if (index != -1) {
            index += event->delta() > 0 ? -1 : 1;
            if (index >= 0 && index < count())
                setCurrentIndex(index);
        }
    }
};


static void removeTileReferences(MapDocument *mapDocument,
                                 std::function<bool(const Cell &)> condition)
{
    QUndoStack *undoStack = mapDocument->undoStack();

    for (Layer *layer : mapDocument->map()->layers()) {
        if (TileLayer *tileLayer = layer->asTileLayer()) {
            const QRegion refs = tileLayer->region(condition);
            if (!refs.isEmpty())
                undoStack->push(new EraseTiles(mapDocument, tileLayer, refs));

        } else if (ObjectGroup *objectGroup = layer->asObjectGroup()) {
            for (MapObject *object : *objectGroup) {
                if (condition(object->cell()))
                    undoStack->push(new RemoveMapObject(mapDocument, object));
            }
        }
    }
}

} // anonymous namespace

TilesetDock::TilesetDock(QWidget *parent):
    QDockWidget(parent),
    mMapDocument(nullptr),
    mTabBar(new WheelEnabledTabBar),
    mViewStack(new QStackedWidget),
    mToolBar(new QToolBar),
    mCurrentTile(nullptr),
    mCurrentTiles(nullptr),
    mNewTileset(new QAction(this)),
    mImportTileset(new QAction(this)),
    mExportTileset(new QAction(this)),
    mEditTileset(new QAction(this)),
    mDeleteTileset(new QAction(this)),
    mTilesetMenuButton(new TilesetMenuButton(this)),
    mTilesetMenu(new QMenu(this)),
    mTilesetActionGroup(new QActionGroup(this)),
    mTilesetMenuMapper(nullptr),
    mEmittingStampCaptured(false),
    mSynchronizingSelection(false)
{
    setObjectName(QLatin1String("TilesetDock"));

    mTabBar->setUsesScrollButtons(true);
    mTabBar->setExpanding(false);

    connect(mTabBar, SIGNAL(currentChanged(int)),
            SLOT(updateActions()));

    QWidget *w = new QWidget(this);

    QHBoxLayout *horizontal = new QHBoxLayout;
    horizontal->setSpacing(0);
    horizontal->addWidget(mTabBar);
    horizontal->addWidget(mTilesetMenuButton);

    QVBoxLayout *vertical = new QVBoxLayout(w);
    vertical->setSpacing(0);
    vertical->setMargin(5);
    vertical->addLayout(horizontal);
    vertical->addWidget(mViewStack);

    horizontal = new QHBoxLayout;
    horizontal->setSpacing(0);
    horizontal->addWidget(mToolBar, 1);
    vertical->addLayout(horizontal);

    mNewTileset->setIcon(QIcon(QLatin1String(":images/16x16/document-new.png")));
    mImportTileset->setIcon(QIcon(QLatin1String(":images/16x16/document-import.png")));
    mExportTileset->setIcon(QIcon(QLatin1String(":images/16x16/document-export.png")));
    mEditTileset->setIcon(QIcon(QLatin1String(":images/16x16/document-properties.png")));
    mDeleteTileset->setIcon(QIcon(QLatin1String(":images/16x16/edit-delete.png")));

    Utils::setThemeIcon(mNewTileset, "document-new");
    Utils::setThemeIcon(mImportTileset, "document-import");
    Utils::setThemeIcon(mExportTileset, "document-export");
    Utils::setThemeIcon(mEditTileset, "document-properties");
    Utils::setThemeIcon(mDeleteTileset, "edit-delete");

    connect(mNewTileset, SIGNAL(triggered()),
            SIGNAL(newTileset()));
    connect(mImportTileset, SIGNAL(triggered()),
            SLOT(importTileset()));
    connect(mExportTileset, SIGNAL(triggered()),
            SLOT(exportTileset()));
    connect(mEditTileset, SIGNAL(triggered()),
            SLOT(editTileset()));
    connect(mDeleteTileset, SIGNAL(triggered()),
            SLOT(removeTileset()));

    mToolBar->addAction(mNewTileset);
    mToolBar->setIconSize(QSize(16, 16));
    mToolBar->addAction(mImportTileset);
    mToolBar->addAction(mExportTileset);
    mToolBar->addAction(mEditTileset);
    mToolBar->addAction(mDeleteTileset);

    mZoomable = new Zoomable(this);
    mZoomComboBox = new QComboBox;
    mZoomable->setComboBox(mZoomComboBox);
    horizontal->addWidget(mZoomComboBox);

    connect(mViewStack, &QStackedWidget::currentChanged,
            this, &TilesetDock::updateCurrentTiles);
    connect(mViewStack, &QStackedWidget::currentChanged,
            this, &TilesetDock::currentTilesetChanged);

    connect(TilesetManager::instance(), SIGNAL(tilesetChanged(Tileset*)),
            this, SLOT(tilesetChanged(Tileset*)));

    auto *documentManager = DocumentManager::instance();

    connect(documentManager, &DocumentManager::tilesetDocumentAdded,
            this, &TilesetDock::tilesetAdded);
    connect(documentManager, &DocumentManager::tilesetDocumentRemoved,
            this, &TilesetDock::tilesetRemoved);

    mTilesetMenuButton->setMenu(mTilesetMenu);
    connect(mTilesetMenu, SIGNAL(aboutToShow()), SLOT(refreshTilesetMenu()));

    setWidget(w);
    retranslateUi();
    setAcceptDrops(true);

    updateTilesets();

    updateActions();
}

TilesetDock::~TilesetDock()
{
    delete mCurrentTiles;
}

void TilesetDock::setMapDocument(MapDocument *mapDocument)
{
    if (mMapDocument == mapDocument)
        return;

    // Hide while we update the tab bar, to avoid repeated layouting
    // But, this causes problems on OS X (issue #1055)
#ifndef Q_OS_OSX
    widget()->hide();
#endif

    setCurrentTiles(nullptr);
    setCurrentTile(nullptr);

    // Clear previous content
//    while (mTabBar->count())
//        mTabBar->removeTab(0);
//    while (mViewStack->count())
//        delete mViewStack->widget(0);

//    mTilesets.clear();

    // Clear all connections to the previous document
//    if (mMapDocument)
//        mMapDocument->disconnect(this);

    mMapDocument = mapDocument;

    if (mMapDocument) {
//        mTilesets = mMapDocument->map()->tilesets();
//        for (int i = 0; i < mTilesets.size(); ++i)
//            createTilesetView(i, mTilesets.at(i).data());

//        connect(mMapDocument, &MapDocument::tilesetAdded,
//                this, &TilesetDock::tilesetAdded);
//        connect(mMapDocument, &MapDocument::tilesetRemoved,
//                this, &TilesetDock::tilesetRemoved);
//        connect(mMapDocument, &MapDocument::tilesetReplaced,
//                this, &TilesetDock::tilesetReplaced);

        if (Object *object = mMapDocument->currentObject())
            if (object->typeId() == Object::TileType)
                setCurrentTile(static_cast<Tile*>(object));
    }

    updateActions();

#ifndef Q_OS_OSX
    widget()->show();
#endif
}

/**
 * Synchronizes the selection with the given stamp. Ignored when the stamp is
 * changing because of a selection change in the TilesetDock.
 */
void TilesetDock::selectTilesInStamp(const TileStamp &stamp)
{
    if (mEmittingStampCaptured)
        return;

    QSet<Tile*> processed;
    QMap<QItemSelectionModel*, QItemSelection> selections;

    for (const TileStampVariation &variation : stamp.variations()) {
        const TileLayer &tileLayer = *variation.tileLayer();
        for (const Cell &cell : tileLayer) {
            if (Tile *tile = cell.tile()) {
                if (processed.contains(tile))
                    continue;

                processed.insert(tile); // avoid spending time on duplicates

                Tileset *tileset = tile->tileset();
                int tilesetIndex = mTilesets.indexOf(tileset->sharedPointer());
                if (tilesetIndex != -1) {
                    TilesetView *view = tilesetViewAt(tilesetIndex);
                    if (!view->model()) // Lazily set up the model
                        setupTilesetModel(view, tileset);

                    const TilesetModel *model = view->tilesetModel();
                    const QModelIndex modelIndex = model->tileIndex(tile);
                    QItemSelectionModel *selectionModel = view->selectionModel();
                    selections[selectionModel].select(modelIndex, modelIndex);
                }
            }
        }
    }

    if (!selections.isEmpty()) {
        mSynchronizingSelection = true;

        // Mark captured tiles as selected
        for (auto i = selections.constBegin(); i != selections.constEnd(); ++i) {
            QItemSelectionModel *selectionModel = i.key();
            const QItemSelection &selection = i.value();
            selectionModel->select(selection, QItemSelectionModel::SelectCurrent);
        }

        // Show/edit properties of all captured tiles
        mMapDocument->setSelectedTiles(processed.toList());

        // Update the current tile (useful for animation and collision editors)
        auto first = selections.begin();
        QItemSelectionModel *selectionModel = first.key();
        const QItemSelection &selection = first.value();
        const QModelIndex currentIndex = selection.first().topLeft();
        if (selectionModel->currentIndex() != currentIndex)
            selectionModel->setCurrentIndex(currentIndex, QItemSelectionModel::NoUpdate);
        else
            currentChanged(currentIndex);

        mSynchronizingSelection = false;
    }
}

void TilesetDock::changeEvent(QEvent *e)
{
    QDockWidget::changeEvent(e);
    switch (e->type()) {
    case QEvent::LanguageChange:
        retranslateUi();
        break;
    default:
        break;
    }
}

void TilesetDock::dragEnterEvent(QDragEnterEvent *e)
{
    const QList<QUrl> urls = e->mimeData()->urls();
    if (!urls.isEmpty() && !urls.at(0).toLocalFile().isEmpty())
        e->accept();
}

void TilesetDock::dropEvent(QDropEvent *e)
{
    QStringList paths;
    for (const QUrl &url : e->mimeData()->urls()) {
        const QString localFile = url.toLocalFile();
        if (!localFile.isEmpty())
            paths.append(localFile);
    }
    if (!paths.isEmpty()) {
        emit tilesetsDropped(paths);
        e->accept();
    }
}

void TilesetDock::currentTilesetChanged()
{
    if (const TilesetView *view = currentTilesetView())
        if (const QItemSelectionModel *s = view->selectionModel())
            setCurrentTile(view->tilesetModel()->tileAt(s->currentIndex()));
}

void TilesetDock::selectionChanged()
{
    updateActions();

    if (!mSynchronizingSelection)
        updateCurrentTiles();
}

void TilesetDock::currentChanged(const QModelIndex &index)
{
    if (!index.isValid())
        return;

    const TilesetModel *model = static_cast<const TilesetModel*>(index.model());
    setCurrentTile(model->tileAt(index));
}

void TilesetDock::updateActions()
{
    bool external = false;
    TilesetView *view = nullptr;
    const int index = mTabBar->currentIndex();

    if (index > -1) {
        view = tilesetViewAt(index);
        if (view) {
            Tileset *tileset = mTilesets.at(index).data();

            if (!view->model()) // Lazily set up the model
                setupTilesetModel(view, tileset);

            mViewStack->setCurrentIndex(index);
            external = tileset->isExternal();
        }
    }

    const bool tilesetIsDisplayed = view != nullptr;
    const bool mapIsDisplayed = mMapDocument != nullptr;

    mNewTileset->setEnabled(mapIsDisplayed);
    mImportTileset->setEnabled(tilesetIsDisplayed && external);
    mExportTileset->setEnabled(tilesetIsDisplayed && !external);
    mEditTileset->setEnabled(tilesetIsDisplayed);
    mDeleteTileset->setEnabled(tilesetIsDisplayed);
}

void TilesetDock::updateCurrentTiles()
{
    TilesetView *view = currentTilesetView();
    if (!view)
        return;

    const QItemSelectionModel *s = view->selectionModel();
    if (!s)
        return;

    const QModelIndexList indexes = s->selection().indexes();
    if (indexes.isEmpty())
        return;

    const QModelIndex &first = indexes.first();
    int minX = first.column();
    int maxX = first.column();
    int minY = first.row();
    int maxY = first.row();

    for (const QModelIndex &index : indexes) {
        if (minX > index.column()) minX = index.column();
        if (maxX < index.column()) maxX = index.column();
        if (minY > index.row()) minY = index.row();
        if (maxY < index.row()) maxY = index.row();
    }

    // Create a tile layer from the current selection
    TileLayer *tileLayer = new TileLayer(QString(), 0, 0,
                                         maxX - minX + 1,
                                         maxY - minY + 1);

    const TilesetModel *model = view->tilesetModel();
    for (const QModelIndex &index : indexes) {
        tileLayer->setCell(index.column() - minX,
                           index.row() - minY,
                           Cell(model->tileAt(index)));
    }

    setCurrentTiles(tileLayer);
}

void TilesetDock::indexPressed(const QModelIndex &index)
{
    TilesetView *view = currentTilesetView();
    if (Tile *tile = view->tilesetModel()->tileAt(index))
        mMapDocument->setCurrentObject(tile);
}

void TilesetDock::tilesetAdded(TilesetDocument *tilesetDocument)
{
    updateTilesets();
    updateActions();    // todo: check if needed
}

void TilesetDock::createTilesetView(int index, TilesetDocument *tilesetDocument)
{
    auto tileset = tilesetDocument->tileset();

    TilesetView *view = new TilesetView;
    // todo: Make sure the view does not crash in read-only mode
    view->setTilesetDocument(tilesetDocument);
    view->setZoomable(mZoomable);

    mTabBar->insertTab(index, tileset->name());
    mViewStack->insertWidget(index, view);

    connect(tilesetDocument, &TilesetDocument::tilesetNameChanged,
            this, &TilesetDock::tilesetNameChanged);
    connect(tilesetDocument, &TilesetDocument::fileNameChanged,
            this, &TilesetDock::updateActions);
    connect(tilesetDocument, &TilesetDocument::tilesetChanged,
            this, &TilesetDock::tilesetChanged);
    connect(tilesetDocument, &TilesetDocument::tileImageSourceChanged,
            this, &TilesetDock::tileImageSourceChanged);
    connect(tilesetDocument, &TilesetDocument::tileAnimationChanged,
            this, &TilesetDock::tileAnimationChanged);
}

void TilesetDock::tilesetChanged(Tileset *tileset)
{
    // Update the affected tileset model, if it exists
    const int index = indexOf(mTilesets, tileset);
    if (index < 0)
        return;

    TilesetView *view = tilesetViewAt(index);

    if (TilesetModel *model = view->tilesetModel()) {
        view->updateBackgroundColor();
        model->tilesetChanged();
    }
}

void TilesetDock::tilesetRemoved(TilesetDocument *tilesetDocument)
{
    updateTilesets();
    updateActions();
}

void TilesetDock::tilesetReplaced(int index, Tileset *tileset)
{
    auto *documentManager = DocumentManager::instance();
    if (auto *tilesetDocument = documentManager->findTilesetDocument(tileset->sharedPointer()))
        tilesetDocument->disconnect(this);

    mTilesets.replace(index, tileset->sharedPointer());

    if (TilesetModel *model = tilesetViewAt(index)->tilesetModel())
        model->setTileset(tileset);

    if (mTabBar->tabText(index) != tileset->name())
        mTabBar->setTabText(index, tileset->name());
}

/**
 * Removes the currently selected tileset.
 */
void TilesetDock::removeTileset()
{
    const int currentIndex = mViewStack->currentIndex();
    if (currentIndex != -1)
        removeTileset(mViewStack->currentIndex());
}

/**
 * Removes the tileset at the given index. Prompting the user when the tileset
 * is in use by the map.
 */
void TilesetDock::removeTileset(int index)
{
    Tileset *tileset = mTilesets.at(index).data();
    const bool inUse = mMapDocument->map()->isTilesetUsed(tileset);

    // If the tileset is in use, warn the user and confirm removal
    if (inUse) {
        QMessageBox warning(QMessageBox::Warning,
                            tr("Remove Tileset"),
                            tr("The tileset \"%1\" is still in use by the "
                               "map!").arg(tileset->name()),
                            QMessageBox::Yes | QMessageBox::No,
                            this);
        warning.setDefaultButton(QMessageBox::Yes);
        warning.setInformativeText(tr("Remove this tileset and all references "
                                      "to the tiles in this tileset?"));

        if (warning.exec() != QMessageBox::Yes)
            return;
    }

    QUndoCommand *remove = new RemoveTileset(mMapDocument, index);
    QUndoStack *undoStack = mMapDocument->undoStack();

    if (inUse) {
        // Remove references to tiles in this tileset from the current map
        auto referencesTileset = [tileset] (const Cell &cell) {
            return cell.tileset() == tileset;
        };
        undoStack->beginMacro(remove->text());
        removeTileReferences(mMapDocument, referencesTileset);
    }
    undoStack->push(remove);
    if (inUse)
        undoStack->endMacro();
}

void TilesetDock::setCurrentTiles(TileLayer *tiles)
{
    if (mCurrentTiles == tiles)
        return;

    delete mCurrentTiles;
    mCurrentTiles = tiles;

    // Set the selected tiles on the map document
    if (tiles) {
        QList<Tile*> selectedTiles;
        for (int y = 0; y < tiles->height(); ++y) {
            for (int x = 0; x < tiles->width(); ++x) {
                const Cell &cell = tiles->cellAt(x, y);
                if (Tile *tile = cell.tile())
                    selectedTiles.append(tile);
            }
        }
        mMapDocument->setSelectedTiles(selectedTiles);

        // Create a tile stamp with these tiles
        Map *map = mMapDocument->map();
        Map *stamp = new Map(map->orientation(),
                             tiles->width(),
                             tiles->height(),
                             map->tileWidth(),
                             map->tileHeight());
        stamp->addLayer(tiles->clone());
        stamp->addTilesets(tiles->usedTilesets());

        mEmittingStampCaptured = true;
        emit stampCaptured(TileStamp(stamp));
        mEmittingStampCaptured = false;
    }
}

void TilesetDock::setCurrentTile(Tile *tile)
{
    if (mCurrentTile == tile)
        return;

    mCurrentTile = tile;
    emit currentTileChanged(tile);

    if (tile)
        mMapDocument->setCurrentObject(tile);
}

void TilesetDock::retranslateUi()
{
    setWindowTitle(tr("Tilesets"));
    mNewTileset->setText(tr("New Tileset"));
    mImportTileset->setText(tr("&Import Tileset"));
    mExportTileset->setText(tr("&Export Tileset As..."));
    mEditTileset->setText(tr("Edit Tile&set"));
    mDeleteTileset->setText(tr("&Remove Tileset"));
}

void TilesetDock::updateTilesets()
{
    // Try to update the list of tilesets with minimal changes


    auto *documentManager = DocumentManager::instance();
    auto tilesetDocuments = documentManager->tilesetDocuments();

    std::sort(tilesetDocuments.begin(),
              tilesetDocuments.end(),
              [](const TilesetDocument *a, const TilesetDocument *b) {
        return a->tileset()->name() < b->tileset()->name();
    });

    QVector<SharedTileset> tilesets;

    int indexNew = 0, indexOld = 0;

    while (indexNew < tilesetDocuments.size()) {
        TilesetDocument *insertionCanditate = tilesetDocuments.at(indexNew);

        if (indexOld < mTilesetDocuments.size()) {
            TilesetDocument *removalCandidate = mTilesetDocuments.at(indexOld);

            if (tilesetDocuments.contains(removalCandidate)) {
                // insert new tileset here
                ++indexOld;
            } else {
                // remove old tileset
                mTilesetDocuments.removeAt(indexOld);
            }
        }


    }


    for (TilesetDocument *tilesetDocument : tilesetDocuments) {
        tilesets.append(tilesetDocument->tileset());
        createTilesetView(mTilesets.size(), tilesetDocument);
    }

    //    tilesetDocument->disconnect(this);

    //    // Delete the related tileset view
    //    const int index = indexOf(mTilesets, tilesetDocument->tileset());
    //    Q_ASSERT(index != -1);

    //    mTilesets.remove(index);
    //    mTabBar->removeTab(index);
    //    delete tilesetViewAt(index);

    //    Tileset *tileset = tilesetDocument->tileset().data();

    //    // Make sure we don't reference this tileset anymore
    //    if (mCurrentTiles) {
    //        // TODO: Don't clean unnecessarily (but first the concept of
    //        //       "current brush" would need to be introduced)
    //        TileLayer *cleaned = static_cast<TileLayer *>(mCurrentTiles->clone());
    //        cleaned->removeReferencesToTileset(tileset);
    //        setCurrentTiles(cleaned);
    //    }
    //    if (mCurrentTile && mCurrentTile->tileset() == tileset)
    //        setCurrentTile(nullptr);


    mTilesets = tilesets;
}

Tileset *TilesetDock::currentTileset() const
{
    const int index = mTabBar->currentIndex();
    if (index == -1)
        return nullptr;

    return mTilesets.at(index).data();
}

TilesetView *TilesetDock::currentTilesetView() const
{
    return static_cast<TilesetView *>(mViewStack->currentWidget());
}

TilesetView *TilesetDock::tilesetViewAt(int index) const
{
    return static_cast<TilesetView *>(mViewStack->widget(index));
}

void TilesetDock::setupTilesetModel(TilesetView *view, Tileset *tileset)
{
    view->setModel(new TilesetModel(tileset, view));

    QItemSelectionModel *s = view->selectionModel();
    connect(s, &QItemSelectionModel::selectionChanged,
            this, &TilesetDock::selectionChanged);
    connect(s, &QItemSelectionModel::currentChanged,
            this, &TilesetDock::currentChanged);
    connect(view, &TilesetView::pressed,
            this, &TilesetDock::indexPressed);
}

void TilesetDock::editTileset()
{
    Tileset *tileset = currentTileset();
    if (!tileset)
        return;

    DocumentManager *documentManager = DocumentManager::instance();
    documentManager->openTileset(tileset->sharedPointer());
}

void TilesetDock::exportTileset()
{
    Tileset *tileset = currentTileset();
    if (!tileset)
        return;

    FormatHelper<TilesetFormat> helper(FileFormat::ReadWrite);

    Preferences *prefs = Preferences::instance();

    QString suggestedFileName = prefs->lastPath(Preferences::ExternalTileset);
    suggestedFileName += QLatin1Char('/');
    suggestedFileName += tileset->name();

    const QLatin1String extension(".tsx");
    if (!suggestedFileName.endsWith(extension))
        suggestedFileName.append(extension);

    // todo: remember last used filter
    QString selectedFilter = TsxTilesetFormat().nameFilter();
    const QString fileName =
            QFileDialog::getSaveFileName(this, tr("Export Tileset"),
                                         suggestedFileName,
                                         helper.filter(), &selectedFilter);

    if (fileName.isEmpty())
        return;

    prefs->setLastPath(Preferences::ExternalTileset,
                       QFileInfo(fileName).path());

    TilesetFormat *format = helper.formatByNameFilter(selectedFilter);
    if (!format)
        return;     // can't happen

    if (format->write(*tileset, fileName)) {
        // todo: Reconsider what these import/export actions will actually do
//        QUndoCommand *command = new SetTilesetFileName(mMapDocument,
//                                                       tileset,
//                                                       fileName);
//        mMapDocument->undoStack()->push(command);
    } else {
        QString error = format->errorString();
        QMessageBox::critical(window(),
                              tr("Export Tileset"),
                              tr("Error saving tileset: %1").arg(error));
    }
}

void TilesetDock::importTileset()
{
    Tileset *tileset = currentTileset();
    if (!tileset)
        return;

    // todo: Reconsider what these import/export actions will actually do
//    QUndoCommand *command = new SetTilesetFileName(mMapDocument,
//                                                   tileset,
//                                                   QString());
//    mMapDocument->undoStack()->push(command);
}

void TilesetDock::tilesetNameChanged(Tileset *tileset)
{
    const int index = indexOf(mTilesets, tileset);
    Q_ASSERT(index != -1);

    mTabBar->setTabText(index, tileset->name());
}

void TilesetDock::tileImageSourceChanged(Tile *tile)
{
    int tilesetIndex = mTilesets.indexOf(tile->tileset()->sharedPointer());
    if (tilesetIndex != -1) {
        TilesetView *view = tilesetViewAt(tilesetIndex);
        if (TilesetModel *model = view->tilesetModel())
            model->tileChanged(tile);
    }
}

void TilesetDock::tileAnimationChanged(Tile *tile)
{
    if (TilesetView *view = currentTilesetView())
        if (TilesetModel *model = view->tilesetModel())
            model->tileChanged(tile);
}

void TilesetDock::refreshTilesetMenu()
{
    mTilesetMenu->clear();

    if (mTilesetMenuMapper) {
        mTabBar->disconnect(mTilesetMenuMapper);
        delete mTilesetMenuMapper;
    }

    mTilesetMenuMapper = new QSignalMapper(this);
    connect(mTilesetMenuMapper, SIGNAL(mapped(int)),
            mTabBar, SLOT(setCurrentIndex(int)));

    const int currentIndex = mTabBar->currentIndex();

    for (int i = 0; i < mTabBar->count(); ++i) {
        QAction *action = new QAction(mTabBar->tabText(i), this);
        action->setCheckable(true);

        mTilesetActionGroup->addAction(action);
        if (i == currentIndex)
            action->setChecked(true);

        mTilesetMenu->addAction(action);
        connect(action, SIGNAL(triggered()), mTilesetMenuMapper, SLOT(map()));
        mTilesetMenuMapper->setMapping(action, i);
    }
}
