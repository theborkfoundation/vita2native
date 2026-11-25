import { createApp, ref, computed, onMounted, onUnmounted } from 'https://esm.sh/vue@3/dist/vue.esm-browser.prod.js';
import {
  Search,
  Pencil,
  Trash2,
  Upload,
  Package,
  KeyRound,
  Image as ImageIcon,
  X,
  CheckCircle2,
  AlertCircle,
  Loader2
} from 'https://esm.sh/lucide-vue-next?deps=vue@3';

function formatBytes(bytes) {
  if (bytes < 1024) return `${bytes} B`;
  if (bytes < 1024 * 1024) return `${(bytes / 1024).toFixed(1)} KB`;
  if (bytes < 1024 * 1024 * 1024) return `${(bytes / (1024 * 1024)).toFixed(1)} MB`;
  return `${(bytes / (1024 * 1024 * 1024)).toFixed(2)} GB`;
}

const App = {
  components: {
    Search,
    Pencil,
    Trash2,
    Upload,
    Package,
    KeyRound,
    ImageIcon,
    X,
    CheckCircle2,
    AlertCircle,
    Loader2
  },
  setup() {
    const tab = ref('library');
    const games = ref([]);
    const search = ref('');
    const loadingGames = ref(true);

    const serverBusy = ref(false);
    const history = ref([]);

    // Import form state
    const pkgFile = ref(null);
    const licFile = ref(null);
    const dragTarget = ref('');
    const uploading = ref(false);
    const trackingImport = ref(false);
    const uploadPct = ref(0);
    const uploadText = ref('');
    const importNote = ref('');
    const importError = ref(false);

    // Edit metadata modal state
    const editing = ref(null);
    const editForm = ref({ name: '', category: '', rating: '', description: '' });
    const artUpload = ref(null);
    const iconUpload = ref(null);
    const screenshotUploads = ref([]);
    const removeArt = ref(false);
    const removeIcon = ref(false);
    const removedScreenshots = ref(new Set());
    const savingMeta = ref(false);
    const metaError = ref('');

    // Delete confirm state
    const deletingGame = ref(null);
    const deletingBusy = ref(false);
    const deleteError = ref('');

    const filteredGames = computed(() => {
      const q = search.value.trim().toLowerCase();
      if (!q) return games.value;
      return games.value.filter(
        (g) =>
          g.name.toLowerCase().includes(q) ||
          g.titleId.toLowerCase().includes(q) ||
          (g.category && g.category.toLowerCase().includes(q))
      );
    });

    async function fetchGames() {
      try {
        const res = await fetch('/api/games', { cache: 'no-store' });
        if (res.ok) {
          games.value = await res.json();
        }
      } finally {
        loadingGames.value = false;
      }
    }

    async function fetchStatus() {
      try {
        const res = await fetch('/api/status', { cache: 'no-store' });
        if (!res.ok) return;
        const data = await res.json();
        const wasBusy = serverBusy.value;
        serverBusy.value = Boolean(data.busy);
        history.value = data.history || [];

        if (trackingImport.value) {
          const latest = history.value[0];
          if (serverBusy.value) {
            if (latest && latest.status === 'running' && latest.statusText) {
              uploadText.value = latest.statusText;
            }
          } else {
            trackingImport.value = false;
            uploadPct.value = 0;
            uploadText.value = '';
            if (latest && latest.status === 'failed') {
              importError.value = true;
              importNote.value = latest.statusText || 'Import failed.';
            } else {
              importError.value = false;
              importNote.value = '';
            }
          }
        }

        if (wasBusy && !serverBusy.value) {
          fetchGames();
        }
      } catch (_) {}
    }

    function onDropFile(kind, e) {
      dragTarget.value = '';
      const f = e.dataTransfer?.files?.[0];
      if (!f) return;
      if (kind === 'pkg') pkgFile.value = f;
      else licFile.value = f;
    }

    function submitImport() {
      if (!pkgFile.value || !licFile.value || uploading.value || serverBusy.value) return;
      const body = new FormData();
      body.append('pkg', pkgFile.value);
      body.append('license', licFile.value);

      uploading.value = true;
      trackingImport.value = false;
      uploadPct.value = 0;
      uploadText.value = 'Uploading...';
      importNote.value = '';
      importError.value = false;

      const xhr = new XMLHttpRequest();
      xhr.open('POST', '/api/import');
      xhr.upload.onprogress = (ev) => {
        if (!ev.lengthComputable) return;
        uploadPct.value = Math.min(100, Math.round((ev.loaded / ev.total) * 100));
        uploadText.value = `${formatBytes(ev.loaded)} / ${formatBytes(ev.total)}`;
      };
      xhr.onload = () => {
        uploading.value = false;
        if (xhr.status >= 200 && xhr.status < 300) {
          trackingImport.value = true;
          uploadPct.value = 100;
          uploadText.value = 'Extracting...';
          pkgFile.value = null;
          licFile.value = null;
          fetchStatus();
        } else {
          uploadPct.value = 0;
          uploadText.value = '';
          importError.value = true;
          try {
            const parsed = JSON.parse(xhr.responseText);
            importNote.value = parsed.error || `Upload failed (${xhr.status})`;
          } catch (_) {
            importNote.value = `Upload failed (${xhr.status})`;
          }
        }
      };
      xhr.onerror = () => {
        uploading.value = false;
        trackingImport.value = false;
        uploadPct.value = 0;
        uploadText.value = '';
        importError.value = true;
        importNote.value = 'Connection lost during upload.';
      };
      xhr.send(body);
    }

    function openEdit(game) {
      editing.value = game;
      editForm.value = {
        name: game.name || '',
        category: game.category || '',
        rating: game.rating || '',
        description: game.description || ''
      };
      artUpload.value = null;
      iconUpload.value = null;
      screenshotUploads.value = [];
      removeArt.value = false;
      removeIcon.value = false;
      removedScreenshots.value = new Set();
      metaError.value = '';
    }

    function toggleRemoveScreenshot(name) {
      const next = new Set(removedScreenshots.value);
      if (next.has(name)) next.delete(name);
      else next.add(name);
      removedScreenshots.value = next;
    }

    async function saveMetadata() {
      if (!editing.value || savingMeta.value) return;
      savingMeta.value = true;
      metaError.value = '';
      try {
        const body = new FormData();
        body.append('name', editForm.value.name);
        body.append('category', editForm.value.category);
        body.append('rating', editForm.value.rating);
        body.append('description', editForm.value.description);
        if (removeArt.value) body.append('removeArt', '1');
        if (removeIcon.value) body.append('removeIcon', '1');
        if (removedScreenshots.value.size > 0) {
          body.append('removeScreenshots', Array.from(removedScreenshots.value).join('\n'));
        }
        if (artUpload.value) body.append('art', artUpload.value);
        if (iconUpload.value) body.append('icon', iconUpload.value);
        for (const f of screenshotUploads.value) {
          body.append('screenshots', f);
        }

        const res = await fetch(`/api/games/${encodeURIComponent(editing.value.titleId)}/meta`, {
          method: 'POST',
          body
        });
        if (!res.ok) {
          const data = await res.json().catch(() => ({}));
          throw new Error(data.error || `Save failed (${res.status})`);
        }
        editing.value = null;
        await fetchGames();
      } catch (err) {
        metaError.value = err.message || 'Could not save metadata.';
      } finally {
        savingMeta.value = false;
      }
    }

    function confirmDelete(game) {
      deletingGame.value = game;
      deleteError.value = '';
    }

    async function performDelete() {
      if (!deletingGame.value || deletingBusy.value) return;
      deletingBusy.value = true;
      deleteError.value = '';
      try {
        const res = await fetch(`/api/games/${encodeURIComponent(deletingGame.value.titleId)}`, {
          method: 'DELETE'
        });
        if (!res.ok) {
          const data = await res.json().catch(() => ({}));
          throw new Error(data.error || `Delete failed (${res.status})`);
        }
        deletingGame.value = null;
        await fetchGames();
      } catch (err) {
        deleteError.value = err.message || 'Could not delete game.';
      } finally {
        deletingBusy.value = false;
      }
    }

    let timer = null;
    onMounted(() => {
      fetchGames();
      fetchStatus();
      timer = setInterval(fetchStatus, 800);
    });
    onUnmounted(() => {
      if (timer) clearInterval(timer);
    });

    return {
      tab,
      games,
      filteredGames,
      search,
      loadingGames,
      serverBusy,
      history,
      pkgFile,
      licFile,
      dragTarget,
      uploading,
      trackingImport,
      uploadPct,
      uploadText,
      importNote,
      importError,
      editing,
      editForm,
      artUpload,
      iconUpload,
      screenshotUploads,
      removeArt,
      removeIcon,
      removedScreenshots,
      savingMeta,
      metaError,
      deletingGame,
      deletingBusy,
      deleteError,
      formatBytes,
      onDropFile,
      submitImport,
      openEdit,
      toggleRemoveScreenshot,
      saveMetadata,
      confirmDelete,
      performDelete
    };
  },
  template: `
    <div class="min-h-screen bg-black text-[#EDEDED]">
      <header class="sticky top-0 z-20 border-b border-[#1F1F1F] bg-black/80 backdrop-blur-md">
        <div class="mx-auto flex max-w-5xl items-center justify-between px-6 py-3.5">
          <div class="flex items-center gap-3">
            <span class="inline-flex h-5 w-5 items-center justify-center text-white">
              <svg viewBox="0 0 16 16" fill="currentColor" class="h-4 w-4"><path d="M8 2L15 14H1L8 2Z"/></svg>
            </span>
            <span class="text-sm font-semibold tracking-tight text-white">vita2native</span>
          </div>
          <nav class="flex items-center gap-1 rounded-lg border border-[#1F1F1F] bg-[#0A0A0A] p-0.5 text-xs font-medium">
            <button
              @click="tab = 'library'"
              class="rounded-md px-3 py-1.5 transition"
              :class="tab === 'library' ? 'bg-[#1F1F1F] text-white' : 'text-[#888888] hover:text-[#EDEDED]'"
            >
              Library
              <span class="ml-1 font-mono text-[11px] text-[#888888]">{{ games.length }}</span>
            </button>
            <button
              @click="tab = 'import'"
              class="rounded-md px-3 py-1.5 transition"
              :class="tab === 'import' ? 'bg-[#1F1F1F] text-white' : 'text-[#888888] hover:text-[#EDEDED]'"
            >
              Import
            </button>
          </nav>
        </div>
      </header>

      <main class="mx-auto max-w-5xl px-6 py-8">
        <!-- Library Tab -->
        <section v-if="tab === 'library'" class="space-y-6">
          <div class="flex flex-col gap-3 sm:flex-row sm:items-center sm:justify-between">
            <h1 class="text-xl font-semibold tracking-tight text-white">Library</h1>
            <div class="relative w-full sm:w-64">
              <Search class="pointer-events-none absolute left-3 top-1/2 h-3.5 w-3.5 -translate-y-1/2 text-[#666666]" />
              <input
                v-model="search"
                type="search"
                placeholder="Filter titles..."
                class="w-full rounded-md border border-[#1F1F1F] bg-[#0A0A0A] py-1.5 pl-8 pr-3 text-sm text-[#EDEDED] placeholder-[#666666] outline-none transition focus:border-[#444444]"
              />
            </div>
          </div>

          <div v-if="loadingGames" class="rounded-lg border border-[#1F1F1F] bg-[#0A0A0A] p-8 text-center text-sm text-[#888888]">
            Loading...
          </div>

          <div v-else-if="filteredGames.length === 0" class="rounded-lg border border-[#1F1F1F] bg-[#0A0A0A] p-10 text-center">
            <p class="text-sm text-[#EDEDED]">No titles found.</p>
          </div>

          <div v-else class="grid grid-cols-1 gap-3 sm:grid-cols-2 lg:grid-cols-3">
            <article
              v-for="game in filteredGames"
              :key="game.titleId"
              class="group flex flex-col justify-between overflow-hidden rounded-lg border border-[#1F1F1F] bg-[#0A0A0A] transition hover:border-[#333333]"
            >
              <div class="flex gap-3.5 p-4">
                <div class="flex h-16 w-16 shrink-0 items-center justify-center overflow-hidden rounded border border-[#1F1F1F] bg-[#111111]">
                  <img
                    v-if="game.iconUrl || game.artUrl"
                    :src="game.iconUrl || game.artUrl"
                    :alt="game.name"
                    class="h-full w-full object-cover"
                  />
                  <ImageIcon v-else class="h-4 w-4 text-[#444444]" />
                </div>
                <div class="min-w-0 flex-1">
                  <h2 class="truncate text-sm font-medium text-white">{{ game.name }}</h2>
                  <p class="mt-0.5 font-mono text-xs text-[#888888]">{{ game.titleId }}</p>
                  <div class="mt-2 flex flex-wrap gap-1.5">
                    <span v-if="game.category" class="rounded border border-[#1F1F1F] bg-[#111111] px-1.5 py-0.5 text-[11px] text-[#A1A1A1]">
                      {{ game.category }}
                    </span>
                    <span v-if="game.rating" class="rounded border border-[#1F1F1F] bg-[#111111] px-1.5 py-0.5 text-[11px] text-[#A1A1A1]">
                      {{ game.rating }}
                    </span>
                  </div>
                </div>
              </div>

              <div class="flex items-center justify-end gap-2 border-t border-[#1A1A1A] px-4 py-3">
                <button
                  @click="openEdit(game)"
                  class="inline-flex items-center gap-1.5 rounded-md border border-[#222222] bg-[#111111] px-2.5 py-1 text-xs font-medium text-[#EDEDED] transition hover:border-[#444444] hover:bg-[#1A1A1A]"
                >
                  <Pencil class="h-3 w-3" />
                  Edit
                </button>
                <button
                  @click="confirmDelete(game)"
                  class="inline-flex items-center gap-1.5 rounded-md border border-[#2A1616] bg-[#160B0B] px-2.5 py-1 text-xs font-medium text-red-400 transition hover:border-red-500/50 hover:bg-red-950/50"
                >
                  <Trash2 class="h-3 w-3" />
                  Delete
                </button>
              </div>
            </article>
          </div>
        </section>

        <!-- Import Tab -->
        <section v-if="tab === 'import'" class="space-y-6">
          <h1 class="text-xl font-semibold tracking-tight text-white">Import</h1>

          <form @submit.prevent="submitImport" class="space-y-5 rounded-lg border border-[#1F1F1F] bg-[#0A0A0A] p-5">
            <div class="grid grid-cols-1 gap-4 sm:grid-cols-2">
              <label
                @dragover.prevent="dragTarget = 'pkg'"
                @dragleave="dragTarget = ''"
                @drop.prevent="onDropFile('pkg', $event)"
                class="relative flex cursor-pointer flex-col gap-1.5 rounded-lg border border-dashed p-5 transition"
                :class="dragTarget === 'pkg' || pkgFile ? 'border-white/50 bg-[#111111]' : 'border-[#262626] bg-black hover:border-[#444444]'"
              >
                <input type="file" accept=".pkg" class="sr-only" @change="pkgFile = $event.target.files[0] || null" />
                <div class="flex items-center gap-2 text-[#888888]">
                  <Package class="h-3.5 w-3.5" />
                  <span class="font-mono text-[11px] uppercase tracking-wider">Package</span>
                </div>
                <span class="text-sm font-medium text-white">.pkg file</span>
                <span class="truncate text-xs text-[#888888]">
                  {{ pkgFile ? pkgFile.name + ' (' + formatBytes(pkgFile.size) + ')' : 'Choose or drop .pkg' }}
                </span>
              </label>

              <label
                @dragover.prevent="dragTarget = 'lic'"
                @dragleave="dragTarget = ''"
                @drop.prevent="onDropFile('lic', $event)"
                class="relative flex cursor-pointer flex-col gap-1.5 rounded-lg border border-dashed p-5 transition"
                :class="dragTarget === 'lic' || licFile ? 'border-white/50 bg-[#111111]' : 'border-[#262626] bg-black hover:border-[#444444]'"
              >
                <input type="file" accept=".bin" class="sr-only" @change="licFile = $event.target.files[0] || null" />
                <div class="flex items-center gap-2 text-[#888888]">
                  <KeyRound class="h-3.5 w-3.5" />
                  <span class="font-mono text-[11px] uppercase tracking-wider">License</span>
                </div>
                <span class="text-sm font-medium text-white">work.bin</span>
                <span class="truncate text-xs text-[#888888]">
                  {{ licFile ? licFile.name + ' (' + formatBytes(licFile.size) + ')' : 'Choose or drop work.bin' }}
                </span>
              </label>
            </div>

            <div v-if="uploading || trackingImport" class="space-y-1.5">
              <div class="flex justify-between text-xs text-[#888888] tabular-nums">
                <span>{{ uploadText }}</span>
                <span class="font-mono">{{ uploadPct }}%</span>
              </div>
              <div class="h-1.5 w-full overflow-hidden rounded-full bg-[#1A1A1A]">
                <div class="h-full bg-white transition-all duration-150" :style="{ width: uploadPct + '%' }"></div>
              </div>
            </div>

            <div class="flex items-center justify-between gap-3">
              <p v-if="importNote" class="inline-flex items-center gap-1.5 text-xs" :class="importError ? 'text-red-400' : 'text-[#888888]'">
                <AlertCircle v-if="importError" class="h-3.5 w-3.5 shrink-0" />
                <span>{{ importNote }}</span>
              </p>
              <span v-else></span>
              <button
                type="submit"
                :disabled="!pkgFile || !licFile || uploading || serverBusy"
                class="inline-flex items-center gap-1.5 rounded-md bg-white px-4 py-2 text-xs font-semibold text-black transition hover:bg-neutral-200 disabled:cursor-not-allowed disabled:opacity-40"
              >
                <Upload class="h-3.5 w-3.5" />
                {{ uploading ? 'Uploading...' : (serverBusy ? 'Busy' : 'Import') }}
              </button>
            </div>
          </form>

          <div v-if="history.length > 0" class="rounded-lg border border-[#1F1F1F] bg-[#0A0A0A] p-5">
            <h2 class="text-xs font-medium uppercase tracking-wider text-[#888888]">History</h2>
            <div class="mt-3 divide-y divide-[#181818]">
              <div v-for="item in history" :key="item.id" class="flex items-center justify-between gap-4 py-2.5 text-sm">
                <span class="truncate text-[#EDEDED]">{{ item.title }}</span>
                <span
                  class="inline-flex shrink-0 items-center gap-1.5 font-mono text-xs"
                  :class="item.status === 'success' ? 'text-emerald-400' : (item.status === 'failed' ? 'text-red-400' : 'text-amber-400')"
                >
                  <CheckCircle2 v-if="item.status === 'success'" class="h-3.5 w-3.5" />
                  <AlertCircle v-else-if="item.status === 'failed'" class="h-3.5 w-3.5" />
                  <Loader2 v-else class="h-3.5 w-3.5 animate-spin" />
                  {{ item.statusText }}
                </span>
              </div>
            </div>
          </div>
        </section>
      </main>

      <!-- Edit Metadata Modal -->
      <div v-if="editing" class="fixed inset-0 z-30 flex items-center justify-center bg-black/80 p-4 backdrop-blur-sm">
        <div class="max-h-[90vh] w-full max-w-xl overflow-y-auto rounded-xl border border-[#262626] bg-[#0A0A0A] p-6 shadow-2xl">
          <div class="flex items-center justify-between border-b border-[#1A1A1A] pb-3">
            <div>
              <h2 class="text-base font-semibold text-white">Edit Metadata</h2>
              <p class="font-mono text-xs text-[#888888]">{{ editing.titleId }}</p>
            </div>
            <button @click="editing = null" class="rounded p-1 text-[#888888] hover:text-white">
              <X class="h-4 w-4" />
            </button>
          </div>

          <form @submit.prevent="saveMetadata" class="mt-4 space-y-4">
            <div>
              <label class="mb-1 block text-xs text-[#888888]">Title Name</label>
              <input v-model="editForm.name" type="text" class="w-full rounded-md border border-[#1F1F1F] bg-black px-3 py-1.5 text-sm text-white outline-none focus:border-[#444444]" />
            </div>

            <div class="grid grid-cols-2 gap-3">
              <div>
                <label class="mb-1 block text-xs text-[#888888]">Category</label>
                <input v-model="editForm.category" type="text" placeholder="Action, RPG..." class="w-full rounded-md border border-[#1F1F1F] bg-black px-3 py-1.5 text-sm text-white outline-none focus:border-[#444444]" />
              </div>
              <div>
                <label class="mb-1 block text-xs text-[#888888]">Rating</label>
                <input v-model="editForm.rating" type="text" placeholder="E, T, M..." class="w-full rounded-md border border-[#1F1F1F] bg-black px-3 py-1.5 text-sm text-white outline-none focus:border-[#444444]" />
              </div>
            </div>

            <div>
              <label class="mb-1 block text-xs text-[#888888]">Description</label>
              <textarea v-model="editForm.description" rows="3" class="w-full rounded-md border border-[#1F1F1F] bg-black px-3 py-1.5 text-sm text-white outline-none focus:border-[#444444]"></textarea>
            </div>

            <div class="grid grid-cols-1 gap-3 sm:grid-cols-2">
              <div class="space-y-2 rounded-md border border-[#1F1F1F] bg-black p-3">
                <div class="flex items-center justify-between">
                  <span class="text-xs font-medium text-[#EDEDED]">Icon</span>
                  <button v-if="editing.hasIcon && !removeIcon" type="button" @click="removeIcon = true" class="text-[11px] text-red-400 hover:underline">Remove</button>
                </div>
                <img v-if="editing.iconUrl && !removeIcon && !iconUpload" :src="editing.iconUrl" class="h-20 w-20 rounded border border-[#1F1F1F] object-cover" />
                <input type="file" accept="image/*" @change="iconUpload = $event.target.files[0] || null; removeIcon = false" class="block w-full text-xs text-[#888888] file:mr-2 file:rounded file:border-0 file:bg-[#1A1A1A] file:px-2 file:py-1 file:text-xs file:text-white" />
              </div>

              <div class="space-y-2 rounded-md border border-[#1F1F1F] bg-black p-3">
                <div class="flex items-center justify-between">
                  <span class="text-xs font-medium text-[#EDEDED]">Hero Art</span>
                  <button v-if="editing.hasArt && !removeArt" type="button" @click="removeArt = true" class="text-[11px] text-red-400 hover:underline">Remove</button>
                </div>
                <img v-if="editing.artUrl && !removeArt && !artUpload" :src="editing.artUrl" class="h-24 w-full rounded border border-[#1F1F1F] object-cover" />
                <input type="file" accept="image/*" @change="artUpload = $event.target.files[0] || null; removeArt = false" class="block w-full text-xs text-[#888888] file:mr-2 file:rounded file:border-0 file:bg-[#1A1A1A] file:px-2 file:py-1 file:text-xs file:text-white" />
              </div>
            </div>

            <div class="space-y-2 rounded-md border border-[#1F1F1F] bg-black p-3">
              <span class="block text-xs font-medium text-[#EDEDED]">Screenshots</span>
              <div v-if="editing.screenshots && editing.screenshots.length" class="flex flex-wrap gap-2">
                <div
                  v-for="shot in editing.screenshots"
                  :key="shot.name"
                  class="relative overflow-hidden rounded border"
                  :class="removedScreenshots.has(shot.name) ? 'border-red-500/70 opacity-40' : 'border-[#1F1F1F]'"
                >
                  <img :src="shot.url" class="h-14 w-24 object-cover" />
                  <button
                    type="button"
                    @click="toggleRemoveScreenshot(shot.name)"
                    class="absolute right-1 top-1 rounded bg-black/80 px-1.5 py-0.5 text-[10px] text-white"
                  >
                    {{ removedScreenshots.has(shot.name) ? 'Undo' : '×' }}
                  </button>
                </div>
              </div>
              <input type="file" accept="image/*" multiple @change="screenshotUploads = Array.from($event.target.files || [])" class="block w-full text-xs text-[#888888] file:mr-2 file:rounded file:border-0 file:bg-[#1A1A1A] file:px-2 file:py-1 file:text-xs file:text-white" />
            </div>

            <p v-if="metaError" class="text-xs text-red-400">{{ metaError }}</p>

            <div class="flex justify-end gap-2 border-t border-[#1A1A1A] pt-3">
              <button type="button" @click="editing = null" class="rounded-md border border-[#222222] px-3 py-1.5 text-xs text-[#EDEDED] hover:bg-[#111111]">Cancel</button>
              <button type="submit" :disabled="savingMeta" class="rounded-md bg-white px-4 py-1.5 text-xs font-semibold text-black hover:bg-neutral-200 disabled:opacity-40">
                {{ savingMeta ? 'Saving...' : 'Save' }}
              </button>
            </div>
          </form>
        </div>
      </div>

      <!-- Delete Confirmation Modal -->
      <div v-if="deletingGame" class="fixed inset-0 z-30 flex items-center justify-center bg-black/80 p-4 backdrop-blur-sm">
        <div class="w-full max-w-md rounded-xl border border-[#262626] bg-[#0A0A0A] p-6">
          <h2 class="text-base font-semibold text-white">Delete {{ deletingGame.name }}?</h2>
          <p class="mt-2 text-xs leading-relaxed text-[#888888]">
            Removes <code class="font-mono text-[#EDEDED]">games/{{ deletingGame.titleId }}</code>. Save files remain untouched.
          </p>
          <p v-if="deleteError" class="mt-2 text-xs text-red-400">{{ deleteError }}</p>
          <div class="mt-5 flex justify-end gap-2">
            <button @click="deletingGame = null" class="rounded-md border border-[#222222] px-3 py-1.5 text-xs text-[#EDEDED] hover:bg-[#111111]">Cancel</button>
            <button @click="performDelete" :disabled="deletingBusy" class="rounded-md bg-red-600 px-3.5 py-1.5 text-xs font-semibold text-white hover:bg-red-500 disabled:opacity-40">
              {{ deletingBusy ? 'Deleting...' : 'Delete' }}
            </button>
          </div>
        </div>
      </div>
    </div>
  `
};

createApp(App).mount('#app');
