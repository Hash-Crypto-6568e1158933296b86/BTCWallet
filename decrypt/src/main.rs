use eframe::egui;
use std::fs;
use rfd::FileDialog;
use base64::{Engine as _, engine::general_purpose};
use aes_gcm::{
    aead::{Aead, KeyInit},
    Aes256Gcm, Key, Nonce,
};
use pbkdf2::pbkdf2_hmac;
use sha2::{Digest, Sha256};

// Tamanhos idênticos ao ESP32
const SALT_LEN: usize = 16;   // NOVO: salt PBKDF2
const IV_LEN:   usize = 12;
const TAG_LEN:  usize = 16;
const KEY_LEN:  usize = 32;
const KEY_CHECK_LEN: usize = 16;

// Deve ser idêntico ao PBKDF2_ITERS do firmware
const PBKDF2_ITER: u32 = 100_000;

fn build_key_password_check(password: &str, salt: &[u8]) -> [u8; KEY_CHECK_LEN] {
    let mut hasher = Sha256::new();
    hasher.update(b"BTCWallet:keyfile:v2");
    hasher.update(salt);
    hasher.update(password.as_bytes());
    let digest = hasher.finalize();

    let mut out = [0u8; KEY_CHECK_LEN];
    out.copy_from_slice(&digest[..KEY_CHECK_LEN]);
    out
}

fn parse_key_file(raw: &str) -> Result<(Vec<u8>, Option<Vec<u8>>), String> {
    let trimmed = raw.trim();

    if !trimmed.contains(':') {
        let salt = general_purpose::STANDARD
            .decode(trimmed)
            .map_err(|e| format!("Erro ao decodificar chave.key (Base64): {e}"))?;
        if salt.len() != SALT_LEN {
            return Err(format!(
                "chave.key inválida: esperado {} bytes de salt, obtido {}",
                SALT_LEN,
                salt.len()
            ));
        }
        return Ok((salt, None));
    }

    let mut salt: Option<Vec<u8>> = None;
    let mut check: Option<Vec<u8>> = None;

    for line in trimmed.lines() {
        let line = line.trim();
        if line.is_empty() {
            continue;
        }
        if let Some(value) = line.strip_prefix("salt:") {
            let decoded = general_purpose::STANDARD
                .decode(value.trim())
                .map_err(|e| format!("Erro ao decodificar salt do chave.key: {e}"))?;
            if decoded.len() != SALT_LEN {
                return Err(format!(
                    "chave.key inválida: esperado {} bytes de salt, obtido {}",
                    SALT_LEN,
                    decoded.len()
                ));
            }
            salt = Some(decoded);
        } else if let Some(value) = line.strip_prefix("check:") {
            let decoded = general_purpose::STANDARD
                .decode(value.trim())
                .map_err(|e| format!("Erro ao decodificar check do chave.key: {e}"))?;
            if decoded.len() != KEY_CHECK_LEN {
                return Err(format!(
                    "chave.key inválida: esperado {} bytes no check, obtido {}",
                    KEY_CHECK_LEN,
                    decoded.len()
                ));
            }
            check = Some(decoded);
        }
    }

    match salt {
        Some(salt) => Ok((salt, check)),
        None => Err("chave.key inválida: campo salt ausente".to_string()),
    }
}

fn main() -> eframe::Result {
    let options = eframe::NativeOptions {
        viewport: egui::ViewportBuilder::default()
            .with_inner_size([700.0, 560.0])
            .with_resizable(true),
        ..Default::default()
    };

    eframe::run_native(
        "BTC Wallet Decryptor",
        options,
        Box::new(|_cc| Ok(Box::new(BtcDecryptApp::default()))),
    )
}

struct BtcDecryptApp {
    file_path:      String,
    key_file_path:  String,
    password:       String,   // senha para PBKDF2 — nunca salva em disco
    decrypted_text: String,
    status:         String,
    error:          String,
}

impl Default for BtcDecryptApp {
    fn default() -> Self {
        Self {
            file_path:      String::new(),
            key_file_path:  String::new(),
            password:       String::new(),
            decrypted_text: String::new(),
            status:         String::new(),
            error:          String::new(),
        }
    }
}

impl eframe::App for BtcDecryptApp {
    fn update(&mut self, ctx: &egui::Context, _frame: &mut eframe::Frame) {
        egui::CentralPanel::default().show(ctx, |ui| {
            ui.heading("🔐 BTC Wallet Decryptor  —  AES-256-GCM + PBKDF2");
            ui.separator();

            // ── Arquivo .enc ──────────────────────────────────────────────────
            ui.horizontal(|ui| {
                ui.label("Arquivo .enc:");
                ui.text_edit_singleline(&mut self.file_path);
                if ui.button("📂 Selecionar").clicked() {
                    if let Some(path) = FileDialog::new()
                        .add_filter("enc", &["enc"])
                        .pick_file()
                    {
                        self.file_path = path.to_string_lossy().to_string();
                        self.decrypted_text.clear();
                        self.error.clear();
                        self.status.clear();
                    }
                }
            });

            ui.add_space(8.0);

            // ── Arquivo chave.key (contém o salt em Base64) ───────────────────
            ui.horizontal(|ui| {
                ui.label("chave.key: ");
                ui.text_edit_singleline(&mut self.key_file_path);
                if ui.button("📂 Selecionar").clicked() {
                    if let Some(path) = FileDialog::new()
                        .add_filter("key", &["key"])
                        .pick_file()
                    {
                        self.key_file_path = path.to_string_lossy().to_string();
                        self.error.clear();
                        self.status.clear();
                    }
                }
            });

            if !self.key_file_path.is_empty() {
                ui.colored_label(
                    egui::Color32::from_rgb(100, 200, 100),
                    "✅ chave.key selecionada",
                );
            } else {
                ui.colored_label(
                    egui::Color32::from_rgb(200, 180, 80),
                    "⚠  Selecione o arquivo chave.key gerado pelo dispositivo",
                );
            }

            ui.add_space(8.0);

            // ── Senha ─────────────────────────────────────────────────────────
            ui.horizontal(|ui| {
                ui.label("Senha:     ");
                ui.add(
                    egui::TextEdit::singleline(&mut self.password)
                        .password(true)
                        .hint_text("senha usada no dispositivo"),
                );
            });

            ui.colored_label(
                egui::Color32::from_rgb(160, 160, 160),
                "ℹ  A chave AES é derivada de: PBKDF2(senha, salt, 100.000 iter)",
            );

            ui.add_space(14.0);

            // ── Botão descriptografar ─────────────────────────────────────────
            if ui
                .add_sized([240.0, 42.0], egui::Button::new("🔓 Descriptografar"))
                .clicked()
            {
                self.decrypt_file();
            }

            ui.add_space(8.0);

            if !self.status.is_empty() {
                ui.colored_label(egui::Color32::GREEN, &self.status);
            }
            if !self.error.is_empty() {
                ui.colored_label(egui::Color32::RED, &self.error);
            }

            ui.separator();
            ui.add_space(8.0);

            // ── Resultado ─────────────────────────────────────────────────────
            ui.label("Resultado:");
            egui::ScrollArea::vertical()
                .max_height(300.0)
                .show(ui, |ui| {
                    ui.add_sized(
                        ui.available_size(),
                        egui::TextEdit::multiline(&mut self.decrypted_text)
                            .font(egui::TextStyle::Monospace)
                            .desired_width(f32::INFINITY),
                    );
                });

            // ── Salvar como .txt ──────────────────────────────────────────────
            if !self.decrypted_text.is_empty() {
                if ui.button("💾 Salvar como .txt").clicked() {
                    if let Some(path) = FileDialog::new()
                        .add_filter("txt", &["txt"])
                        .set_file_name("wallet_decrypted.txt")
                        .save_file()
                    {
                        match fs::write(&path, &self.decrypted_text) {
                            Ok(_)  => self.status = format!("Salvo em: {}", path.display()),
                            Err(e) => self.error  = format!("Erro ao salvar: {}", e),
                        }
                    }
                }
            }
        });
    }
}

impl BtcDecryptApp {
    fn decrypt_file(&mut self) {
        self.error.clear();
        self.status.clear();
        self.decrypted_text.clear();

        // ── Validações básicas ────────────────────────────────────────────────
        if self.file_path.is_empty() {
            self.error = "Selecione um arquivo .enc".to_string();
            return;
        }
        if self.key_file_path.is_empty() {
            self.error = "Selecione o arquivo chave.key".to_string();
            return;
        }
        if self.password.is_empty() {
            self.error = "Digite a senha usada no dispositivo".to_string();
            return;
        }

        // ── Lê arquivo cifrado ────────────────────────────────────────────────
        let enc_data = match fs::read(&self.file_path) {
            Ok(d)  => d,
            Err(e) => { self.error = format!("Erro ao ler .enc: {}", e); return; }
        };

        let min_size = SALT_LEN + IV_LEN + TAG_LEN;
        if enc_data.len() <= min_size {
            self.error = format!(
                "Arquivo .enc muito pequeno ({} bytes; mínimo esperado: {} bytes).",
                enc_data.len(), min_size + 1
            );
            return;
        }

        // ── Lê chave.key — contém Base64(salt) ───────────────────────────────
        let raw_key_file = match fs::read_to_string(&self.key_file_path) {
            Ok(s)  => s.trim().to_string(),
            Err(e) => { self.error = format!("Erro ao ler chave.key: {}", e); return; }
        };

        let (salt_from_file, password_check) = match parse_key_file(&raw_key_file) {
            Ok(parts) => parts,
            Err(e) => {
                self.error = e;
                return;
            }
        };

        // ── Extrai salt, IV, TAG e ciphertext ─────────────────────────────────
        // Formato: [SALT 16][IV 12][TAG 16][ciphertext N]
        let salt_in_file = &enc_data[..SALT_LEN];
        let iv            = &enc_data[SALT_LEN..SALT_LEN + IV_LEN];
        let tag           = &enc_data[SALT_LEN + IV_LEN..SALT_LEN + IV_LEN + TAG_LEN];
        let ciphertext    = &enc_data[SALT_LEN + IV_LEN + TAG_LEN..];

        // Verifica consistência: salt no arquivo .enc deve bater com chave.key
        if salt_in_file != salt_from_file.as_slice() {
            self.error =
                "❌ O salt do arquivo .enc não corresponde ao chave.key selecionado.\n\
                 Use o chave.key gerado junto com este .enc.".to_string();
            return;
        }

        if let Some(expected_check) = password_check {
            let actual_check = build_key_password_check(&self.password, salt_in_file);
            if actual_check.as_slice() != expected_check.as_slice() {
                self.error =
                    "❌ A senha digitada não corresponde ao chave.key selecionado.".to_string();
                return;
            }
        }

        // ── Deriva chave via PBKDF2-HMAC-SHA256 ──────────────────────────────
        let mut key_bytes = [0u8; KEY_LEN];
        pbkdf2_hmac::<Sha256>(
            self.password.as_bytes(),
            salt_in_file,
            PBKDF2_ITER,
            &mut key_bytes,
        );

        // ── Descriptografa AES-256-GCM ────────────────────────────────────────
        // aes-gcm espera ciphertext + tag concatenados
        let mut cipher_with_tag = ciphertext.to_vec();
        cipher_with_tag.extend_from_slice(tag);

        let key    = Key::<Aes256Gcm>::from_slice(&key_bytes);
        let nonce  = Nonce::from_slice(iv);
        let cipher = Aes256Gcm::new(key);

        match cipher.decrypt(nonce, cipher_with_tag.as_ref()) {
            Ok(plain) => {
                match String::from_utf8(plain) {
                    Ok(text) => {
                        self.decrypted_text = text;
                        self.status =
                            "✅ Descriptografado com sucesso! (AES-256-GCM + PBKDF2-SHA256)"
                                .to_string();
                    }
                    Err(_) => {
                        self.error =
                            "Dados descriptografados não são texto UTF-8 válido.".to_string();
                    }
                }
            }
            Err(_) => {
                // GCM não distingue senha errada de arquivo adulterado — por design
                self.error =
                    "❌ Falha na autenticação GCM: senha incorreta ou arquivo adulterado.\n\
                     Verifique a senha e confirme que está usando o chave.key correto."
                        .to_string();
            }
        }
    }
}
