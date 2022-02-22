#include "mainwindow.h"
#include "Patch.h"
#include "WorkerThread.h"
#include "quazip.h"
#include "resourcebin.h"
#include "resourceentrymodel.h"
#include "resourceentryproxymodel.h"
#include "ui_mainwindow.h"
#include <ChronoCrypto.h>
#include <JlCompress.h>
#include <QDirIterator>
#include <QFileDialog>
#include <QFontDatabase>
#include <QMessageBox>
#include <QPainter>
#include <QSortFilterProxyModel>
#include <QTreeWidget>

// todo
// check patch valid
// delete and rename not in loop
// ressource bin - graceful on close
// status messages background job
// stop background job

MainWindow::MainWindow(QWidget* parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{

    // apply dark theme
    QFile f(":/qdarkstyle/dark/style.qss");
    if (!f.exists()) {
        qDebug() << "Unable to set stylesheet, file not found";
    } else {
        f.open(QFile::ReadOnly | QFile::Text);
        QTextStream ts(&f);
        qApp->setStyleSheet(ts.readAll());
    }
    f.close();

    // setup ui and models
    ui->setupUi(this);
    hidePreviews();
    ui->progressBar->setHidden(true);

    QMenu* tableViewMenu = new QMenu(this);

    ResourceEntryModel* model = new ResourceEntryModel(this);
    ResourceEntryProxyModel* proxyModel = new ResourceEntryProxyModel(this);
    proxyModel->setSourceModel(model);
    ui->tableView->setModel(proxyModel);
    ui->tableView->horizontalHeader()->setDefaultAlignment(Qt::AlignLeft | Qt::AlignVCenter);

    // filter entries
    connect(ui->lineEdit, &QLineEdit::textChanged, proxyModel, &ResourceEntryProxyModel::setCurrentSearchString);
    connect(ui->lineEdit, &QLineEdit::textChanged, ui->tableView->selectionModel(), &QItemSelectionModel::clear);
    connect(ui->checkBox, &QCheckBox::stateChanged, proxyModel, &ResourceEntryProxyModel::setOnlyModified);
    connect(ui->checkBox, &QCheckBox::stateChanged, ui->tableView->selectionModel(), &QItemSelectionModel::clear);

    // worker thread for extracting
    connect(&this->workerThread, &WorkerThread::started, ui->progressBar, &QProgressBar::show);
    connect(&this->workerThread, &WorkerThread::progressRangeChanged, ui->progressBar, &QProgressBar::setRange);
    connect(&this->workerThread, &WorkerThread::progressValueChanged, ui->progressBar, &QProgressBar::setValue);
    connect(&this->workerThread, &WorkerThread::finished, ui->progressBar, &QProgressBar::hide);
    connect(this, &MainWindow::patchLoaded, this, [=]() {
        this->ui->actionSave->setEnabled(true);
        this->ui->lineEdit->setText("");
        this->ui->checkBox->setChecked(true);
    });
    connect(this, &MainWindow::modificationUnset, this, [=]() {
        this->ui->actionSave->setEnabled(!this->patchMap.empty());
        ui->tableView->selectionModel()->clear();
        this->ui->checkBox->setChecked(false);
    });

    // ressource.bin loaded - populate table
    connect(this, &MainWindow::ressourceBin_loaded, this, [=]() {
        ui->tableView->selectionModel()->clearSelection();
        ui->actionExtract_All->setEnabled(true);
        ui->actionLoad_Patch->setEnabled(true);
        ui->lineEdit->setEnabled(true);
        ui->lineEdit->setText("");
        ui->plainTextEdit->hide();
        ui->imagePreview->hide();
        ui->imagePreview2->hide();
        this->ui->checkBox->setChecked(false);
        this->ui->actionSave->setEnabled(false);
        auto& entrty_list = this->ressourceBin->getEntries();
        model->setModelData(&entrty_list);
        ui->tableView->sortByColumn(0, Qt::AscendingOrder);
        ui->statusbar->showMessage(QString("Loaded %1 files").arg(entrty_list.size()));
        this->setWindowTitle(QString("ChronoMod - %1").arg(QString::fromStdString(this->ressourceBin->get_path())));
    });

    // preview selected  entry
    connect(ui->tableView->selectionModel(), &QItemSelectionModel::selectionChanged, this, [=]() {
        auto selected_rows = ui->tableView->selectionModel()->selectedRows();

        if (selected_rows.empty()) {
            hidePreviews();
            return;
        }

        if (selected_rows.size() != 1) {
            hidePreviews();
            ui->imagePreview->setText(QString("%1 files selected").arg(selected_rows.size()));
            ui->imagePreview->show();
            return;
        }

        auto& entry = this->ressourceBin->getEntries()[proxyModel->mapToSource(selected_rows[0]).row()];

        auto fileName = QString::fromStdString(entry.path);
        if (fileName.endsWith(".txt")) {
            auto data = this->ressourceBin->extract(entry);
            hidePreviews();
            ui->plainTextEdit->setPlainText(QString::fromUtf8(data.data(), data.size()));
            ui->plainTextEdit->show();
            return;
        }

        if (fileName.endsWith(".png") || fileName.endsWith(".bmp")) {
            bool png = fileName.endsWith(".png");
            auto data = this->ressourceBin->extract(entry);
            QPixmap image;
            image.loadFromData((const unsigned char*)data.data(), data.size(), png ? "PNG" : "BMP");
            image = image.scaled(250, 250, Qt::KeepAspectRatio, Qt::SmoothTransformation);
            hidePreviews();
            ui->imagePreview->setPixmap(image);
            ui->imagePreview->show();
            if (entry.hasReplacement) {
                qDebug() << "has replacement!";
                Patch& patch = this->patchMap[entry.path];
                QFile patchFile(QString::fromStdString(patch.path));
                patchFile.open(QFile::ReadOnly);
                QPixmap image2;
                image2.loadFromData(patchFile.readAll(), png ? "PNG" : "BMP");
                patchFile.close();
                image2 = image2.scaled(250, 250, Qt::KeepAspectRatio, Qt::SmoothTransformation);
                ui->imagePreview2->setPixmap(image2);
                ui->imagePreview2->show();
            }
            return;
        }

        if (fileName.startsWith("string_")) {
            auto data = this->ressourceBin->extract(entry);
            auto decrypted = decrypt_file_with_key(decryption_key.data(), data.data(), data.size());
            QByteArray byteArray(decrypted.data(), decrypted.size());
            int font_id = QFontDatabase::addApplicationFontFromData(byteArray);
            assert(font_id != -1);
            QStringList font_families = QFontDatabase::applicationFontFamilies(font_id);
            qDebug() << font_families;

            QFont newFont(font_families[0], 14);
            hidePreviews();
            ui->fontPreview->setFont(newFont);
            QString previewText = "Lorem ipsum dolor sit amet, consetetur sadipscing elitr, sed diam nonumy eirmod tempor invidunt ut labore et dolore magna aliquyam erat, sed diam voluptua. At vero eos et accusam et justo duo dolores et ea rebum. Stet clita kasd gubergren, no sea takimata sanctus est Lorem ipsum dolor sit amet.";
            ui->fontPreview->setText(previewText);
            ui->fontPreview->show();

            if (entry.hasReplacement) {
                qDebug() << "has replacement!";
                Patch& patch = this->patchMap[entry.path];
                QFile patchFile(QString::fromStdString(patch.path));
                patchFile.open(QFile::ReadOnly);
                QByteArray fileByteArray = patchFile.readAll();
                auto decrypted = decrypt_file_with_key(decryption_key.data(), fileByteArray.data(), patchFile.size());
                patchFile.close();
                QByteArray byteArray(decrypted.data(), decrypted.size());
                int font_id = QFontDatabase::addApplicationFontFromData(byteArray);
                assert(font_id != -1);
                QStringList font_families = QFontDatabase::applicationFontFamilies(font_id);
                qDebug() << font_families;

                QFont newFont(font_families[0], 14);
                ui->fontPreview2->setFont(newFont);
                ui->fontPreview2->setText(previewText);
                ui->fontPreview2->show();
            }
            return;
        }

        hidePreviews();
        ui->imagePreview->setText("No preview");
        ui->imagePreview->show();
        return;
    });

    auto showContextMenu = [=](const QPoint& clickPosition) {
        QItemSelectionModel* select = this->ui->tableView->selectionModel();
        if (!select->hasSelection()) {
            return;
        }

        tableViewMenu->clear();

        auto selectedRows = select->selectedRows();

        QString extract_label = selectedRows.size() == 1 ? "Extract..." : QString("Extract %1 files...").arg(selectedRows.size());
        QAction* extract_action = new QAction(extract_label, tableViewMenu);
        tableViewMenu->addAction(extract_action);
        connect(extract_action, &QAction::triggered, this, [=] {
            std::vector<ResourceEntry> entries;
            for (auto& selectedRow : selectedRows) {
                auto& entry = this->ressourceBin->getEntries()[proxyModel->mapToSource(selectedRow).row()];
                entries.push_back(entry);
            }
            extract_entries(entries);
        });
        if (selectedRows.size() == 1) {
            int entry_index = proxyModel->mapToSource(selectedRows[0]).row();

            QAction* replace_action = new QAction("Replace...", tableViewMenu);
            tableViewMenu->addAction(replace_action);
            connect(replace_action, &QAction::triggered, this, [=] {
                QString fileName = QFileDialog::getOpenFileName(this, "Open replacement...", "C:\\Users\\james\\Downloads\\chrono_trigger");
                qDebug() << fileName;
                if (fileName.isEmpty()) {
                    return;
                }

                auto& entry = this->ressourceBin->getEntries()[entry_index];
                // encryt string files
                if (QString::fromStdString(entry.path).startsWith("string_")) {

                    QFile patchFile(fileName);
                    std::vector<char> patchBuffer(patchFile.size());
                    patchFile.open(QIODevice::ReadOnly);
                    patchFile.read(patchBuffer.data(), patchBuffer.size());
                    patchFile.close();
                    auto encrypted = encrypt_file_with_key(decryption_key.data(), patchBuffer.data(), patchBuffer.size());

                    QString finalPath = QDir(tempDir.path()).filePath(QString::fromStdString(entry.path));
                    QFile decryptedPatchFile(finalPath);
                    decryptedPatchFile.open(QFile::WriteOnly);
                    decryptedPatchFile.write(encrypted.data(), encrypted.size());
                    decryptedPatchFile.close();
                    fileName = finalPath;
                }
                Patch patch(fileName.toStdString());
                entry.hasReplacement = true;
                this->patchMap[entry.path] = patch;

                emit this->patchLoaded();
            });
            QAction* unset_action = new QAction("Remove replacement", tableViewMenu);
            unset_action->setEnabled(false);
            tableViewMenu->addAction(unset_action);
            if (this->ressourceBin->getEntries()[entry_index].hasReplacement) {
                unset_action->setEnabled(true);
                connect(unset_action, &QAction::triggered, this, [=] {
                    auto& entry = this->ressourceBin->getEntries()[entry_index];

                    entry.hasReplacement = false;
                    this->patchMap.erase(entry.path);

                    emit this->modificationUnset();
                });
            }
        }

        tableViewMenu->popup(clickPosition);
    };

    // double click on table entry
    connect(ui->tableView, &QTableView::doubleClicked, this, [=]() {
        QPoint globalCursorPos = QCursor::pos();
        showContextMenu(globalCursorPos);
    });
    // right click on table entry or context menu key
    connect(ui->tableView, &QTableView::customContextMenuRequested, this, [=](const QPoint& clickPositionRelative) {
        QPoint globalCursorPos = this->ui->tableView->viewport()->mapToGlobal(clickPositionRelative);
        showContextMenu(globalCursorPos);
    });
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::hidePreviews()
{
    ui->plainTextEdit->hide();
    ui->imagePreview->hide();
    ui->imagePreview2->hide();
    ui->fontPreview->hide();
    ui->fontPreview2->hide();
}

void MainWindow::loadRessourceBin(const QString& filepath)
{
    try {
        auto ressourceBin = std::make_shared<ResourceBin>(filepath.toStdString());
        ressourceBin->init();
        this->ressourceBin = std::move(ressourceBin);
        emit ressourceBin_loaded();
    } catch (const std::runtime_error& error) {
        QMessageBox::critical(this, "Error",
            error.what(),
            QMessageBox::Close);
    }
}

void MainWindow::on_actionOpen_Archive_triggered()
{

    QString chronoFileName = QFileDialog::getOpenFileName(this, "Open Chrono Trigger.exe...", "F:\\SteamLibrary\\steamapps\\common\\Chrono Trigger", "Exe files (*.exe);;All files (*)");
    qDebug() << chronoFileName;
    if (chronoFileName.isEmpty()) {
        return;
    }

    QFile chronoFile(chronoFileName);
    std::vector<char> chronoBuffer(chronoFile.size());
    chronoFile.open(QIODevice::ReadOnly);
    chronoFile.read(chronoBuffer.data(), chronoBuffer.size());
    chronoFile.close();

    decryption_key = get_key(chronoBuffer.data());

    QString fileName = QFileDialog::getOpenFileName(this, "Open Ressource.bin...", "C:\\Users\\james\\Downloads\\chrono_trigger", "BIN files (*.bin);;All files (*)");
    qDebug() << fileName;
    if (fileName.isEmpty()) {
        return;
    }
    loadRessourceBin(fileName);
}

void MainWindow::on_actionExtract_All_triggered()
{
    extract_entries(this->ressourceBin->getEntries());
}

void MainWindow::extract_entries(const std::vector<ResourceEntry>& entries)
{

    if (entries.size() > 1) {

        if (this->workerThread.isRunning()) {
            QMessageBox::warning(this, "Job in progress...",
                "Wait for previous job to finish!",
                QMessageBox::Close);
            return;
        }

        QString extract_dir = QFileDialog::getExistingDirectory(this, "Choose Directory");
        if (extract_dir.isEmpty()) {
            return;
        }

        this->workerThread.doWork([=] {
            emit this->workerThread.progressRangeChanged(0, entries.size());
            int count = 0;
            for (auto& entry : entries) {

                QString finalPath = QDir(extract_dir).filePath(QString::fromStdString(entry.path));
                //qDebug() << finalPath;

                QDir finalDirectory(QFileInfo(finalPath).absolutePath());

                if (!finalDirectory.mkpath(finalDirectory.path())) {
                    throw std::runtime_error("file could not be created");
                }

                auto ret = this->ressourceBin->extract(entry);

                // decrypt string files
                if (QString::fromStdString(entry.path).startsWith("string_")) {
                    auto decrypted = decrypt_file_with_key(decryption_key.data(), ret.data(), ret.size());
                    ret = std::move(decrypted);
                }
                QFile file(finalPath);
                file.open(QIODevice::WriteOnly);
                file.write(ret.data(), ret.size());
                file.close();
                emit this->workerThread.progressValueChanged(++count);
            }
        });

        this->workerThread.start();
        return;
    }

    QString fileName = QFileInfo(QString::fromStdString(entries[0].path)).fileName();
    QString saveName = QFileDialog::getSaveFileName(this, "Extract File", fileName);
    if (saveName.isEmpty()) {
        return;
    }
    auto ret = this->ressourceBin->extract(entries[0]);

    // decrypt string files
    if (QString::fromStdString(entries[0].path).startsWith("string_")) {
        auto decrypted = decrypt_file_with_key(decryption_key.data(), ret.data(), ret.size());
        ret = std::move(decrypted);
    }
    QFile file(saveName);
    file.open(QIODevice::WriteOnly);
    file.write(ret.data(), ret.size());
    file.close();
    this->ui->statusbar->showMessage(QString("%1 extracted").arg(fileName));
}

void MainWindow::on_actionLoad_Patch_triggered()
{

    QString fileName = QFileDialog::getOpenFileName(this, "Open Patch...", "C:\\Users\\james\\Downloads\\chrono_trigger", "CTP files (*.ctp);;All files (*)");
    qDebug() << fileName;
    if (fileName.isEmpty()) {
        return;
    }
    if (this->workerThread.isRunning()) {
        QMessageBox::warning(this, "Job in progress...",
            "Wait for previous job to finish!",
            QMessageBox::Close);
        return;
    }

    QString finalPath = QDir(tempDir.path()).filePath(QString::number(patchMap.size()));
    qDebug() << "Final path: " << finalPath;

    this->workerThread.doWork([=] {
        emit this->workerThread.progressRangeChanged(0, 0);

        QDir finalDirectory(finalPath);

        if (!finalDirectory.mkpath(finalDirectory.path())) {
            throw std::runtime_error("file could not be created");
        }

        JlCompress::extractDir(fileName, finalPath);

        finalDirectory.setFilter(QDir::Files | QDir::NoDotAndDotDot | QDir::NoSymLinks);
        QDirIterator it(finalDirectory, QDirIterator::Subdirectories);

        while (it.hasNext()) {
            it.next();
            QFileInfo Info = it.fileInfo();
            std::string absolutePath = Info.absoluteFilePath().toStdString();
            std::string relativePath = finalDirectory.relativeFilePath(Info.absoluteFilePath()).toStdString();

            bool entry_found = false;
            for (auto& entry : this->ressourceBin->getEntries()) {
                if (entry.path == relativePath) {
                    qDebug() << QString::fromStdString(relativePath) << " found!";
                    entry_found = true;
                    Patch patch(absolutePath);
                    entry.hasReplacement = true;
                    this->patchMap[entry.path] = patch;
                    break;
                }
            }
            if (!entry_found) {
                qDebug() << QString::fromStdString(relativePath) << "not found! :(";
            }
        }

        emit this->patchLoaded();
    });

    this->workerThread.start();
}

void MainWindow::reload()
{
    QString current_file = QString::fromStdString(this->ressourceBin->get_path());
    loadRessourceBin(current_file);
}

void MainWindow::on_actionSave_triggered()
{

    if (this->workerThread.isRunning()) {
        QMessageBox::warning(this, "Job in progress...",
            "Wait for previous job to finish!",
            QMessageBox::Close);
        return;
    }

    this->workerThread.doWork([=] {
        QString new_path = QDir(tempDir.path()).filePath("resources.bin");
        auto& entries = this->ressourceBin->getEntries();
        emit(this->workerThread.progressRangeChanged(0, entries.size()));
        QElapsedTimer timer;
        timer.start();
        this->ressourceBin->create_with_modifications(patchMap, new_path.toStdString(), [=](int entry_num) {
            emit(this->workerThread.progressValueChanged(entry_num));
        });
        qDebug() << "The operation took" << timer.elapsed() << "milliseconds";
        QFileInfo current_file(QString::fromStdString(this->ressourceBin->get_path()));
        QFileInfo backup_file(QString::fromStdString(this->ressourceBin->get_path() + ".bak"));

        qDebug() << current_file.absoluteFilePath();
        qDebug() << backup_file.absoluteFilePath();

        if (!backup_file.exists()) {
            qDebug() << "create backup file";
            QFile::copy(current_file.absoluteFilePath(), backup_file.absoluteFilePath());
        } else {
            qDebug() << "backup file exists";
        }

        ressourceBin->close();
        while (!QFile::remove(current_file.absoluteFilePath())) {
            qDebug() << "remove error";
            QThread::sleep(1);
        }
        while (!QFile::rename(new_path, current_file.absoluteFilePath())) {
            qDebug() << "rename error";
            QThread::sleep(1);
        }

        reload();
    });

    this->workerThread.start();
}
