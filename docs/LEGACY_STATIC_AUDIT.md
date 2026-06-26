# Legacy static function audit

Generated from `src/midi-ble-rtd.c` after the current cleanup pass.

## Static functions still present

- line 78:static char *keyfile_get_string_default(GKeyFile *kf, const char *group, const char *key, const char *fallback) {
- line 88:static bool keyfile_get_bool_default(GKeyFile *kf, const char *group, const char *key, bool fallback) {
- line 98:static bool load_config(Config *cfg, const char *path) {
- line 135:static void free_config(Config *cfg) {
- line 145:static bool uuid_equal(const char *a, const char *b) {
- line 149:static bool string_contains_casefold(const char *haystack, const char *needle) {
- line 161:static GVariant *get_managed_objects(App *app) {
- line 165:static bool string_array_contains_uuid(GVariant *array, const char *uuid) {
- line 181:static char *find_device(App *app) {
- line 254:static bool get_device_bool_property(App *app, const char *property, bool *out) {
- line 258:static bool set_device_trusted(App *app) {
- line 262:static bool pair_device(App *app) {
- line 266:static bool connect_device(App *app) {
- line 270:static bool wait_services_resolved(App *app, int timeout_ms) {
- line 274:static char *find_ble_midi_service(App *app) {
- line 278:static char *find_ble_midi_characteristic(App *app) {
- line 286:static bool alsa_init(App *app) {
- line 295:static void alsa_emit_midi_byte(App *app, uint8_t byte) {
- line 317:static int midi_data_len(uint8_t status) {
- line 347:static void ble_midi_decode_packet(App *app, const uint8_t *p, size_t len) {
- line 425:static uint16_t ble_midi_timestamp_13bit(void) {
- line 430:static bool ble_midi_write_packet(App *app, const uint8_t *midi, size_t len) {
- line 469:static void print_midi_bytes(const char *prefix, const uint8_t *bytes, size_t len) {
- line 476:static gboolean alsa_rx_poll_cb(gpointer user_data) {
- line 523:static void on_properties_changed(GDBusConnection *connection, const gchar *sender_name,
- line 554:static bool start_notify(App *app) {
- line 581:static void app_cleanup(App *app) {
- line 598:static void print_usage(const char *argv0) {

## Notes

- Functions still used only by the renamed legacy daemon entrypoint should not be moved blindly.
- Functions used by the orchestrator through the legacy include are candidates for extraction.
- Prefer one extraction per commit.
- Do not mix this cleanup with stats/top RX/TX telemetry changes.
