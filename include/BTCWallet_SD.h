// =============================================
// BTCWallet_SD.h - AES-256-GCM (mbedTLS)
// =============================================
// FORMATO DO .enc:
//   [16 bytes salt aleatório       ]  ← NOVO: protege contra ataques de dicionário
//   [12 bytes IV  aleatório  (GCM) ]
//   [16 bytes TAG de autenticação  ]
//   [N  bytes dados cifrados       ]
//
// DERIVAÇÃO DE CHAVE:
//   PBKDF2-HMAC-SHA256(senha, salt, 100000 iterações) → 32 bytes (AES-256)
//
// chave.key:
//   format:btcwallet-key-v2
//   salt:<Base64(salt)>
//   check:<Base64(verificador da senha)>
//   O sal é necessário para descriptografar; a senha nunca é exportada.
//
// Segurança:
//   - PBKDF2 com 100.000 iterações: ~0,5 s no ESP32; GPUs ganham muito menos
//   - Salt de 16 bytes: elimina rainbow tables e paralelismo entre arquivos
//   - AES-256-GCM: cifra autenticada, detecta adulteração
//   - IV aleatório: cada .enc é único mesmo com mesma senha/conteúdo
// =============================================

#pragma once
#include <FS.h>
#include <SD.h>
#include <SPI.h>
#include <base64.h>
#include "mbedtls/md.h"

#include "mbedtls/aes.h"
#include "mbedtls/gcm.h"
#include "mbedtls/sha256.h"
#include "mbedtls/pkcs5.h"

#define SD_CS    5
#define SD_SCK   18
#define SD_MISO  19
#define SD_MOSI  23

#define AES_KEY_BYTES    32   // AES-256
#define GCM_IV_BYTES     12   // IV padrão GCM
#define GCM_TAG_BYTES    16   // Tag de autenticação GCM
#define SALT_BYTES       16   // Salt PBKDF2
#define KEY_CHECK_BYTES  16   // Verificador curto da senha ligado ao salt
#define PBKDF2_ITERS  100000  // Iterações — ~0,5 s no ESP32 @ 240 MHz

SPIClass sdSPI(HSPI);

// -----------------------------------------------------------------------------
// Gera bytes aleatórios com o hardware RNG do ESP32
// -----------------------------------------------------------------------------
static void generateRandom(uint8_t* buf, size_t len) {
  for (size_t i = 0; i < len; i++)
    buf[i] = (uint8_t)(esp_random() & 0xFF);
}

// -----------------------------------------------------------------------------
// Deriva chave AES-256 via PBKDF2-HMAC-SHA256
//   senha  → string digitada pelo usuário
//   salt   → 16 bytes aleatórios (salvos junto com o .enc)
//   key    → 32 bytes resultantes (chave AES)
// Retorna true em caso de sucesso
// -----------------------------------------------------------------------------
static bool deriveKey(const String& password,
                      const uint8_t salt[SALT_BYTES],
                      uint8_t key[AES_KEY_BYTES]) {
  // mbedtls_pkcs5_pbkdf2_hmac_ext é a API disponível no ESP32 Arduino framework

mbedtls_md_context_t mdCtx;
mbedtls_md_init(&mdCtx);
mbedtls_md_setup(&mdCtx, mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), 1);

int ret = mbedtls_pkcs5_pbkdf2_hmac(
    &mdCtx,
    (const uint8_t*)password.c_str(), password.length(),
    salt, SALT_BYTES,
    PBKDF2_ITERS,
    AES_KEY_BYTES,
    key
);
mbedtls_md_free(&mdCtx);

  if (ret != 0) {
    Serial.printf("ERRO deriveKey: PBKDF2 falhou (%d)\n", ret);
    return false;
  }
  return true;
}

// -----------------------------------------------------------------------------
// Cifra com AES-256-GCM
// Saída: iv (12) + tag (16) + ciphertext (mesmo tamanho do plaintext)
// Nota: salt NÃO entra aqui — já foi escrito antes pela função chamadora
// Retorna tamanho de (iv + tag + ciphertext) ou 0 em caso de erro
// -----------------------------------------------------------------------------
static size_t aesGcmEncrypt(const uint8_t* plain, size_t plainLen,
                             const uint8_t key[AES_KEY_BYTES],
                             const uint8_t iv[GCM_IV_BYTES],
                             uint8_t* out) {
  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);

  int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (ret != 0) {
    mbedtls_gcm_free(&gcm);
    return 0;
  }

  uint8_t* cipherOut = out + GCM_IV_BYTES + GCM_TAG_BYTES;
  uint8_t  tag[GCM_TAG_BYTES];

  ret = mbedtls_gcm_crypt_and_tag(
    &gcm,
    MBEDTLS_GCM_ENCRYPT,
    plainLen,
    iv,  GCM_IV_BYTES,
    NULL, 0,
    plain,
    cipherOut,
    GCM_TAG_BYTES,
    tag
  );

  mbedtls_gcm_free(&gcm);
  if (ret != 0) return 0;

  // Monta: [IV | TAG | ciphertext]  (salt já escrito pelo chamador)
  memcpy(out,                iv,  GCM_IV_BYTES);
  memcpy(out + GCM_IV_BYTES, tag, GCM_TAG_BYTES);

  return GCM_IV_BYTES + GCM_TAG_BYTES + plainLen;
}

// -----------------------------------------------------------------------------
// Decifra com AES-256-GCM
// Entrada (in): [IV(12) | TAG(16) | ciphertext]  (salt já consumido antes)
// Retorna tamanho do plaintext, ou 0 se falhar
// -----------------------------------------------------------------------------
static size_t aesGcmDecrypt(const uint8_t* in, size_t inLen,
                             const uint8_t key[AES_KEY_BYTES],
                             uint8_t* plain) {
  if (inLen <= GCM_IV_BYTES + GCM_TAG_BYTES) return 0;

  const uint8_t* iv         = in;
  const uint8_t* tag        = in + GCM_IV_BYTES;
  const uint8_t* ciphertext = in + GCM_IV_BYTES + GCM_TAG_BYTES;
  size_t         cipherLen  = inLen - GCM_IV_BYTES - GCM_TAG_BYTES;

  mbedtls_gcm_context gcm;
  mbedtls_gcm_init(&gcm);

  int ret = mbedtls_gcm_setkey(&gcm, MBEDTLS_CIPHER_ID_AES, key, 256);
  if (ret != 0) { mbedtls_gcm_free(&gcm); return 0; }

  ret = mbedtls_gcm_auth_decrypt(
    &gcm,
    cipherLen,
    iv,  GCM_IV_BYTES,
    NULL, 0,
    tag, GCM_TAG_BYTES,
    ciphertext,
    plain
  );

  mbedtls_gcm_free(&gcm);
  return (ret == 0) ? cipherLen : 0;
}

static void buildKeyPasswordCheck(const String& password,
                                  const uint8_t salt[SALT_BYTES],
                                  uint8_t out[KEY_CHECK_BYTES]) {
  static const char domain[] = "BTCWallet:keyfile:v2";
  uint8_t digest[32];
  mbedtls_sha256_context ctx;
  mbedtls_sha256_init(&ctx);
  mbedtls_sha256_starts(&ctx, 0);
  mbedtls_sha256_update(&ctx, (const uint8_t*)domain, strlen(domain));
  mbedtls_sha256_update(&ctx, salt, SALT_BYTES);
  mbedtls_sha256_update(&ctx, (const uint8_t*)password.c_str(), password.length());
  mbedtls_sha256_finish(&ctx, digest);
  mbedtls_sha256_free(&ctx);
  memcpy(out, digest, KEY_CHECK_BYTES);
}

// -----------------------------------------------------------------------------
// Gera /chave.key no SD
// Conteúdo:
//   format:btcwallet-key-v2
//   salt:<base64>
//   check:<base64>
// O salt é necessário para re-derivar a chave; a senha permanece só na cabeça
// do usuário e nunca é exportada.
// ATENÇÃO: saveKeyFile() deve ser chamado ANTES de saveWalletToSD() para que
// ambos usem o mesmo salt.  O salt é gerado aqui e armazenado globalmente.
// -----------------------------------------------------------------------------
static uint8_t g_salt[SALT_BYTES];                 // salt gerado em saveKeyFile(), reutilizado em saveWalletToSD()
static uint8_t g_keyPasswordCheck[KEY_CHECK_BYTES];
static bool    g_saltReady = false;
static bool    g_keyPasswordCheckReady = false;

bool saveKeyFile(const String& password) {
  if (password.length() == 0) {
    Serial.println("ERRO saveKeyFile: senha vazia");
    return false;
  }

  // Gera salt aleatório e guarda globalmente
  generateRandom(g_salt, SALT_BYTES);
  g_saltReady = true;

  buildKeyPasswordCheck(password, g_salt, g_keyPasswordCheck);
  g_keyPasswordCheckReady = true;

  String encodedSalt = base64::encode(g_salt, SALT_BYTES);
  String encodedCheck = base64::encode(g_keyPasswordCheck, KEY_CHECK_BYTES);
  encodedSalt.trim();
  encodedCheck.trim();
  Serial.println("-> chave.key salt: " + encodedSalt);

  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
  if (!SD.begin(SD_CS, sdSPI)) {
    Serial.println("ERRO: SD falhou ao gerar chave.key");
    sdSPI.end();
    return false;
  }

  if (SD.exists("/chave.key")) SD.remove("/chave.key");

  File f = SD.open("/chave.key", FILE_WRITE);
  if (!f) {
    Serial.println("ERRO: Falha ao criar chave.key");
    SD.end(); sdSPI.end();
    return false;
  }

  f.println("format:btcwallet-key-v2");
  f.println("salt:" + encodedSalt);
  f.println("check:" + encodedCheck);
  f.close();
  SD.end(); sdSPI.end();

  Serial.println("OK: chave.key salvo! (salt PBKDF2 + verificador da senha)");
  return true;
}

// -----------------------------------------------------------------------------
// Salva carteira com AES-256-GCM + PBKDF2 + salt
// Formato do arquivo: [SALT(16) | IV(12) | TAG(16) | ciphertext]
// -----------------------------------------------------------------------------
bool saveWalletToSD(const String& mnemonic,   const String& passphrase,
                    const String& addrP2PKH,  const String& addrP2SH,
                    const String& addrBech32, const String& encPassword,
                    int account = 0) {

  if (encPassword.length() == 0) {
    Serial.println("ERRO saveWalletToSD: senha vazia");
    return false;
  }
  if (!g_saltReady) {
    Serial.println("ERRO saveWalletToSD: chame saveKeyFile() antes para gerar o salt");
    return false;
  }
  if (!g_keyPasswordCheckReady) {
    Serial.println("ERRO saveWalletToSD: verificador da senha indisponivel");
    return false;
  }

  uint8_t currentCheck[KEY_CHECK_BYTES];
  buildKeyPasswordCheck(encPassword, g_salt, currentCheck);
  if (memcmp(currentCheck, g_keyPasswordCheck, KEY_CHECK_BYTES) != 0) {
    Serial.println("ERRO saveWalletToSD: a senha atual nao corresponde ao chave.key gerado");
    Serial.println("      Gere um novo chave.key se trocar a senha.");
    return false;
  }

  // Monta conteúdo plaintext
  String content = "=== BITCOIN HD WALLET - BACKUP ENCRYPTED ===\n";
  content += "Data: " + String(__DATE__) + " " + String(__TIME__) + "\n";
  content += "KDF: PBKDF2-HMAC-SHA256  iter=" + String(PBKDF2_ITERS) + "\n";
  content += "=========================================\n\n";

  int wordCount = 1;
  for (int i = 0; i < (int)mnemonic.length(); i++)
    if (mnemonic[i] == ' ') wordCount++;

  content += "MNEMONIC (" + String(wordCount) + " palavras):\n" + mnemonic + "\n\n";
  content += "PASSPHRASE: " + (passphrase.length() > 0 ? passphrase : "(nenhuma)") + "\n\n";
  content += "DERIVACAO (PATHS):\n";
  content += "BIP44  : m/44'/0'/0'/0/" + String(account) + "\n";
  content += "BIP49  : m/49'/0'/0'/0/" + String(account) + "\n";
  content += "BIP84  : m/84'/0'/0'/0/" + String(account) + "\n\n";
  content += "ENDERECOS:\n";
  content += "BIP44  (Legacy) : " + addrP2PKH  + "\n";
  content += "BIP49  (P2SH)   : " + addrP2SH   + "\n";
  content += "BIP84  (SegWit) : " + addrBech32  + "\n\n";
  content += "=== FIM DO BACKUP ===";

  size_t plainLen = content.length();
  // Espaço: salt + iv + tag + ciphertext
  size_t bufLen   = SALT_BYTES + GCM_IV_BYTES + GCM_TAG_BYTES + plainLen;

  uint8_t* outBuf = (uint8_t*)malloc(bufLen);
  if (!outBuf) {
    Serial.println("ERRO: malloc falhou");
    return false;
  }

  // Deriva chave com PBKDF2 usando o salt já gerado
  uint8_t key[AES_KEY_BYTES];
  if (!deriveKey(encPassword, g_salt, key)) {
    free(outBuf);
    return false;
  }

  // Gera IV aleatório
  uint8_t iv[GCM_IV_BYTES];
  generateRandom(iv, GCM_IV_BYTES);

  // Posição do bloco iv+tag+ciphertext no buffer (logo após o salt)
  uint8_t* gcmOut = outBuf + SALT_BYTES;

  size_t gcmLen = aesGcmEncrypt(
    (const uint8_t*)content.c_str(), plainLen,
    key, iv,
    gcmOut
  );

  if (gcmLen == 0) {
    Serial.println("ERRO: AES-GCM encrypt falhou");
    free(outBuf);
    return false;
  }

  // Copia salt para o início do buffer
  memcpy(outBuf, g_salt, SALT_BYTES);
  size_t outLen = SALT_BYTES + gcmLen;

  // Salva no SD
  Serial.println("-> Salvando carteira AES-256-GCM + PBKDF2...");
  sdSPI.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);

  if (!SD.begin(SD_CS, sdSPI)) {
    Serial.println("ERRO: SD falhou");
    free(outBuf);
    return false;
  }

  String filename = "/wallet_" + String(millis()) + ".enc";
  File file = SD.open(filename, FILE_WRITE);

  if (!file) {
    Serial.println("ERRO: Nao conseguiu criar arquivo");
    SD.end(); free(outBuf);
    return false;
  }

  file.write(outBuf, outLen);
  file.close();
  SD.end(); sdSPI.end();
  free(outBuf);

  Serial.println("OK: Carteira AES-256-GCM + PBKDF2 salva: " + filename);
  Serial.printf("   plaintext=%d bytes  arquivo=%d bytes\n", plainLen, outLen);
  Serial.printf("   (salt=%d + iv=%d + tag=%d + cipher=%d)\n",
    SALT_BYTES, GCM_IV_BYTES, GCM_TAG_BYTES, plainLen);
  return true;
}
