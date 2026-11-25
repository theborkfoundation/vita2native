(() => {
  const form = document.getElementById('import-form');
  const pkgInput = document.getElementById('pkg-input');
  const licInput = document.getElementById('lic-input');
  const pkgDrop = document.getElementById('pkg-drop');
  const licDrop = document.getElementById('lic-drop');
  const pkgMeta = document.getElementById('pkg-meta');
  const licMeta = document.getElementById('lic-meta');
  const submitBtn = document.getElementById('submit-btn');
  const formNote = document.getElementById('form-note');
  const progressWrap = document.getElementById('progress-wrap');
  const progressLabel = document.getElementById('progress-label');
  const progressPct = document.getElementById('progress-pct');
  const progressFill = document.getElementById('progress-fill');
  const historyList = document.getElementById('history-list');
  const serverState = document.getElementById('server-state');

  let uploading = false;
  let serverBusy = false;

  function formatBytes(bytes) {
    if (bytes < 1024) return bytes + ' B';
    if (bytes < 1024 * 1024) return (bytes / 1024).toFixed(1) + ' KB';
    if (bytes < 1024 * 1024 * 1024) return (bytes / (1024 * 1024)).toFixed(1) + ' MB';
    return (bytes / (1024 * 1024 * 1024)).toFixed(2) + ' GB';
  }

  function updateSelectionUI() {
    const pkgFile = pkgInput.files[0];
    const licFile = licInput.files[0];

    pkgDrop.classList.toggle('selected', Boolean(pkgFile));
    licDrop.classList.toggle('selected', Boolean(licFile));
    pkgMeta.textContent = pkgFile ? `${pkgFile.name} (${formatBytes(pkgFile.size)})` : 'Choose or drop a .pkg file';
    licMeta.textContent = licFile ? `${licFile.name} (${formatBytes(licFile.size)})` : 'Choose or drop work.bin';

    submitBtn.disabled = uploading || serverBusy || !pkgFile || !licFile;
    if (!uploading && !serverBusy) {
      formNote.className = 'form-note';
      formNote.textContent = pkgFile && licFile ? 'Ready to upload and import.' : 'Select both files to begin.';
    }
  }

  function wireDropZone(dropEl, inputEl) {
    ['dragenter', 'dragover'].forEach((evt) => {
      dropEl.addEventListener(evt, (e) => {
        e.preventDefault();
        dropEl.classList.add('dragover');
      });
    });
    ['dragleave', 'drop'].forEach((evt) => {
      dropEl.addEventListener(evt, (e) => {
        e.preventDefault();
        dropEl.classList.remove('dragover');
      });
    });
    dropEl.addEventListener('drop', (e) => {
      if (e.dataTransfer && e.dataTransfer.files && e.dataTransfer.files.length > 0) {
        inputEl.files = e.dataTransfer.files;
        updateSelectionUI();
      }
    });
    inputEl.addEventListener('change', updateSelectionUI);
  }

  wireDropZone(pkgDrop, pkgInput);
  wireDropZone(licDrop, licInput);

  function renderHistory(items) {
    historyList.textContent = '';
    if (!items || items.length === 0) {
      const empty = document.createElement('div');
      empty.className = 'history-empty';
      empty.textContent = 'No imports in this session yet.';
      historyList.appendChild(empty);
      return;
    }
    for (const item of items) {
      const row = document.createElement('div');
      row.className = 'history-item';

      const left = document.createElement('div');
      left.className = 'history-left';

      const tag = document.createElement('span');
      tag.className = 'channel-tag';
      tag.textContent = item.channel;

      const title = document.createElement('span');
      title.className = 'history-title';
      title.textContent = item.title;

      left.appendChild(tag);
      left.appendChild(title);

      const status = document.createElement('span');
      status.className = `history-status ${item.status}`;
      status.textContent = item.statusText;

      row.appendChild(left);
      row.appendChild(status);
      historyList.appendChild(row);
    }
  }

  async function pollStatus() {
    try {
      const res = await fetch('/api/status', { cache: 'no-store' });
      if (!res.ok) return;
      const data = await res.json();
      serverBusy = Boolean(data.busy);
      serverState.textContent = serverBusy ? 'Importing...' : 'Ready';
      serverState.classList.toggle('busy', serverBusy);
      renderHistory(data.history || []);
      updateSelectionUI();
    } catch (_) {
      // Ignore transient network errors while polling.
    }
  }

  form.addEventListener('submit', (e) => {
    e.preventDefault();
    if (uploading || serverBusy) return;

    const pkgFile = pkgInput.files[0];
    const licFile = licInput.files[0];
    if (!pkgFile || !licFile) return;

    const body = new FormData();
    body.append('pkg', pkgFile);
    body.append('license', licFile);

    uploading = true;
    progressWrap.classList.remove('hidden');
    progressLabel.textContent = 'Uploading package...';
    progressPct.textContent = '0%';
    progressFill.style.width = '0%';
    formNote.className = 'form-note';
    formNote.textContent = 'Uploading files to device...';
    updateSelectionUI();

    const xhr = new XMLHttpRequest();
    xhr.open('POST', '/api/import');

    xhr.upload.onprogress = (ev) => {
      if (!ev.lengthComputable) return;
      const pct = Math.min(100, Math.round((ev.loaded / ev.total) * 100));
      progressPct.textContent = pct + '%';
      progressFill.style.width = pct + '%';
      progressLabel.textContent = `Uploading (${formatBytes(ev.loaded)} / ${formatBytes(ev.total)})`;
    };

    xhr.onload = () => {
      uploading = false;
      if (xhr.status >= 200 && xhr.status < 300) {
        progressLabel.textContent = 'Upload complete. Extracting on device...';
        progressPct.textContent = '100%';
        progressFill.style.width = '100%';
        formNote.className = 'form-note success';
        formNote.textContent = 'Upload received. Check Import History below for progress.';
        pkgInput.value = '';
        licInput.value = '';
        pollStatus();
      } else {
        let errMsg = `Upload failed (HTTP ${xhr.status})`;
        try {
          const parsed = JSON.parse(xhr.responseText);
          if (parsed.error) errMsg = parsed.error;
        } catch (_) {}
        formNote.className = 'form-note error';
        formNote.textContent = errMsg;
      }
      updateSelectionUI();
    };

    xhr.onerror = () => {
      uploading = false;
      formNote.className = 'form-note error';
      formNote.textContent = 'Connection lost during upload.';
      updateSelectionUI();
    };

    xhr.send(body);
  });

  pollStatus();
  setInterval(pollStatus, 800);
})();
