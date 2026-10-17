<script setup lang="ts">
import { computed, ref } from "vue";
import { t } from "../i18n";
import { ownYaml, text, type ModelEntry, type Plugin, type PluginBuildSetting } from "../model/plugins";
import { usePluginsStore } from "../stores/plugins";
import type { Screen } from "../types";

const props = defineProps<{ plugin: Plugin; screen: Screen; setting: PluginBuildSetting }>();
const plugins = usePluginsStore();
const url = ref("");
const error = ref("");
const desired = computed(() => plugins.modelsOn(props.screen, props.plugin, props.setting));
const have = computed(() => plugins.installedOn(props.screen, props.plugin.id));
const installed = computed(() => have.value?.installed_build_values?.[props.setting.id] || []);
const failed = computed(() => plugins.retryAvailable(props.screen, props.plugin) ? have.value?.failed_update?.build_values?.[props.setting.id] : undefined);
const pending = computed(() => have.value?.state === "building" || (have.value?.build_revision != null
  && have.value.build_revision !== have.value.installed_build_revision));
const label = (model: ModelEntry) => model.label || text(props.setting.catalogue.find((entry) => entry.url === model.url)?.label || {}) || model.url;
function add(source: string) {
  const value = source.trim();
  if (!value) return;
  if (desired.value.some((model) => model.url === value)) { error.value = t("editor.plugins.models.duplicate"); return; }
  if (desired.value.length >= 16) { error.value = t("editor.plugins.models.limit"); return; }
  const preset = props.setting.catalogue.find((entry) => entry.url === value);
  plugins.setModels(props.screen, props.plugin, props.setting, [...desired.value, { ...(preset ? { id: preset.id } : {}), url: value }]);
  error.value = "";
  url.value = "";
}
function remove(source?: string) {
  plugins.setModels(props.screen, props.plugin, props.setting, source ? desired.value.filter((model) => model.url !== source) : []);
  error.value = "";
}
</script>

<template>
  <section class="plugin-models" :data-model-setting="setting.id" :data-screen="screen.id">
    <h4>{{ text(setting.label) }}<template v-if="screen.name"> · {{ screen.name }}</template></h4>
    <p v-if="setting.hint" class="pd-room">{{ text(setting.hint) }}</p>
    <p v-if="ownYaml(screen)" class="pd-misfit" role="alert">{{ t("editor.plugins.models.managed_required") }}</p>
    <p class="pd-label">{{ t("editor.plugins.models.available") }}</p>
    <ul class="model-catalogue">
      <li v-for="preset in setting.catalogue" :key="preset.id">
        <span>{{ text(preset.label) }}</span>
        <button type="button" class="btn quiet" :data-add-model="preset.id" :disabled="desired.some((model) => model.url === preset.url)"
          @click="add(preset.url)">{{ desired.some((model) => model.url === preset.url)
            ? t(have?.build_confirmed && installed.some((model) => model.url === preset.url) ? 'editor.plugins.models.installed' : 'editor.plugins.models.added')
            : t('editor.plugins.models.add_to_screen') }}</button>
      </li>
    </ul>
    <div class="field model-url">
      <label class="f-label" :for="`model-url-${screen.id}-${setting.id}`">{{ t("editor.plugins.models.url") }}</label>
      <input :id="`model-url-${screen.id}-${setting.id}`" v-model="url" type="url" autocomplete="off" spellcheck="false" placeholder="https://…/model.json" @keydown.enter.prevent="add(url)" />
      <button type="button" class="btn quiet" data-add-url :disabled="!url.trim()" @click="add(url)">{{ t("editor.plugins.models.add") }}</button>
    </div>
    <p v-if="error" class="pd-misfit" role="alert">{{ error }}</p>
    <p class="pd-label">{{ t("editor.plugins.models.desired") }}</p>
    <ul class="model-desired">
      <li v-for="model in desired" :key="model.url">
        <span><b>{{ label(model) }}</b><small>{{ model.url }}</small></span>
        <button type="button" class="btn quiet" :data-remove-model="model.url" @click="remove(model.url)">{{ t("editor.plugins.models.remove") }}</button>
      </li>
    </ul>
    <p v-if="!desired.length" class="pd-room" data-manual-only>{{ t("editor.plugins.models.manual_only") }}</p>
    <p v-if="plugins.modelChoiceRequired(screen, plugin, setting)" class="pd-misfit" data-model-required role="alert">{{ t("editor.plugins.models.required") }}</p>
    <p v-if="plugins.setupError(screen, plugin)" class="pd-misfit" role="alert">{{ plugins.setupError(screen, plugin) }}</p>
    <div class="pd-buttons">
      <button v-if="desired.length" type="button" class="btn quiet" data-remove-all @click="remove()">{{ t("editor.plugins.models.remove_all") }}</button>
      <button v-if="plugins.setupChanged(screen, plugin)" type="button" class="btn quiet" data-cancel-models @click="plugins.forgetDrafts(screen, plugin)">{{ t("editor.common.cancel") }}</button>
    </div>
    <p class="pd-room">{{ t("editor.plugins.models.apply_note") }}</p>
    <div v-if="have" class="model-installed" data-installed-models>
      <p class="pd-label">{{ t(have.build_confirmed ? 'editor.plugins.models.installed' : 'editor.plugins.models.unconfirmed') }}</p>
      <p v-if="pending" class="pd-room" data-model-pending>{{ t("editor.plugins.models.pending") }}</p>
      <ul><li v-for="model in installed" :key="model.url">{{ label(model) }}</li></ul>
      <p v-if="!installed.length" class="pd-room">{{ t("editor.plugins.models.manual_only") }}</p>
    </div>
    <div v-if="failed" class="model-failed" data-failed-models>
      <p class="pd-label">{{ t("editor.plugins.models.failed") }}</p>
      <ul><li v-for="model in failed" :key="model.url">{{ label(model) }}</li></ul>
      <p v-if="!failed.length" class="pd-room">{{ t("editor.plugins.models.manual_only") }}</p>
    </div>
  </section>
</template>

<style scoped>
.plugin-models { margin-block: 1rem; }
h4 { margin-block: 0 .5rem; }
ul { list-style: none; padding: 0; }
.model-catalogue li, .model-desired li { display: flex; gap: .75rem; align-items: center; justify-content: space-between; margin-block: .5rem; }
.model-desired span { min-width: 0; }
.model-desired small { display: block; overflow-wrap: anywhere; }
.model-url input { width: 100%; }
.model-url button { margin-top: .5rem; }
.model-installed, .model-failed { border-top: 1px solid var(--line); margin-top: 1rem; padding-top: .5rem; }
</style>
