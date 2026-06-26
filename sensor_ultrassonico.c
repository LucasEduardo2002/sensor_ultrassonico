#include "pico/stdlib.h"
#include "ssd1306/ssd1306.h"
#include "ssd1306/ssd1306_fonts.h"
#include <stdio.h>

#define TRIG_PIN 9
#define ECHO_PIN 8
#define ECHO_TIMEOUT_US 30000
#define BUFFER_SIZE 5

// Buffer circular e flags para média móvel e filtros
float buffer_distancia[BUFFER_SIZE];
int indice_buffer = 0;
bool buffer_inicializado = false;
bool hist_mediana_inicializado = false;

// Função para medir distância
float medir_distancia_cm() {
  gpio_put(TRIG_PIN, 0);
  sleep_us(2);

  gpio_put(TRIG_PIN, 1);
  sleep_us(10);
  gpio_put(TRIG_PIN, 0);

  // Espera ECHO subir
  absolute_time_t timeout = make_timeout_time_us(ECHO_TIMEOUT_US);
  while (gpio_get(ECHO_PIN) == 0) {
    if (absolute_time_diff_us(get_absolute_time(), timeout) <= 0) {
      return -1.0f;
    }
  }

  absolute_time_t start = get_absolute_time();

  // Espera ECHO descer
  timeout = make_timeout_time_us(ECHO_TIMEOUT_US);
  while (gpio_get(ECHO_PIN) == 1) {
    if (absolute_time_diff_us(get_absolute_time(), timeout) <= 0) {
      return -1.0f;
    }
  }

  absolute_time_t end = get_absolute_time();

  int64_t tempo = absolute_time_diff_us(start, end);

  // Distância em cm
  float distancia = (tempo * 0.0343) / 2;

  return distancia;
}

// Função para calibrar a leitura bruta do sensor ultrassônico

float calibrar_distancia(float distancia_bruta) {
  if (distancia_bruta < 0.0f) {
    return distancia_bruta;
  }
  return 1.100413f * distancia_bruta - 2.17f;
}

// Filtra leituras fora dos limites do sensor ou com variações impossíveis
float filtrar_e_validar(float nova_leitura) {
  static float ultima_leitura_valida = -1.0f;

  // Para o nosso tanque (altura_sensor = 77cm), qualquer leitura acima de 100cm
  // é espúria.
  if (nova_leitura < 2.0f || nova_leitura > 100.0f) {
    if (ultima_leitura_valida > 0.0f) {
      return ultima_leitura_valida;
    }
    return -1.0f; // Sem leitura estável anterior
  }

  if (ultima_leitura_valida < 0.0f) {
    ultima_leitura_valida = nova_leitura;
    return nova_leitura;
  }

  // Filtra picos de variação extremamente rápidos (ex: mais de 7cm por
  // ciclo/segundo)
  float diferenca = nova_leitura - ultima_leitura_valida;
  const float max_variacao = 7.0f;
  if (diferenca > max_variacao) {
    nova_leitura = ultima_leitura_valida + max_variacao;
  } else if (diferenca < -max_variacao) {
    nova_leitura = ultima_leitura_valida - max_variacao;
  }

  ultima_leitura_valida = nova_leitura;
  return nova_leitura;
}

// Filtro de Mediana para remover ruídos isolados (outliers)
float calcular_mediana(float nova_leitura) {
  static float historico[BUFFER_SIZE];
  static int idx = 0;

  if (!hist_mediana_inicializado) {
    for (int i = 0; i < BUFFER_SIZE; i++) {
      historico[i] = nova_leitura;
    }
    hist_mediana_inicializado = true;
  }

  historico[idx] = nova_leitura;
  idx = (idx + 1) % BUFFER_SIZE;

  // Copia e ordena os valores temporariamente
  float temp[BUFFER_SIZE];
  for (int i = 0; i < BUFFER_SIZE; i++) {
    temp[i] = historico[i];
  }

  // Bubble Sort
  for (int i = 0; i < BUFFER_SIZE - 1; i++) {
    for (int j = i + 1; j < BUFFER_SIZE; j++) {
      if (temp[i] > temp[j]) {
        float t = temp[i];
        temp[i] = temp[j];
        temp[j] = t;
      }
    }
  }

  // Retorna a mediana (valor central)
  return temp[BUFFER_SIZE / 2];
}

// Função para calcular média móvel
float calcular_media_movel(float nova_leitura) {
  if (!buffer_inicializado) {
    for (int i = 0; i < BUFFER_SIZE; i++) {
      buffer_distancia[i] = nova_leitura;
    }
    buffer_inicializado = true;
  }

  buffer_distancia[indice_buffer] = nova_leitura;
  indice_buffer = (indice_buffer + 1) % BUFFER_SIZE;

  float soma = 0.0f;
  for (int i = 0; i < BUFFER_SIZE; i++) {
    soma += buffer_distancia[i];
  }

  return soma / BUFFER_SIZE;
}

float calcular_volume_interpolado(float altura_leite) {
  if (altura_leite <= 0.0f) {
    return 0.0f;
  }
  // Zona 1: Abaixo do chanfro lateral (0 a 24 cm)
  // Inclui a inclinação do fundo e o canal central (media ponderada de ~9.48 L/cm)
  if (altura_leite <= 24.0f) {
    return 9479.16f * altura_leite;
  }
  // Zona 2: Início do chanfro lateral até a marca de 250L (24 a 26.37 cm)
  if (altura_leite <= 26.37f) {
    return 227500.0f + 9493.67f * (altura_leite - 24.0f);
  }
  // Zona 3: Da marca de 250L até a marca de 550L (26.37 a 57.5 cm)
  // O volume cresce um pouco mais rápido (~9.64 L/cm) pelo volume extra do chanfro
  if (altura_leite <= 57.5f) {
    return 250000.0f + 9637.00f * (altura_leite - 26.37f);
  }
  // Limite máximo de segurança (550L)
  return 550000.0f;
}

float volumeLeite(float distancia) {
  // Altura do sensor em relação ao ponto zero de calibração (em cm)
  const float altura_sensor = 78.0f;

  // Altura máxima do leite para a capacidade máxima de 550L (57.5 cm)
  const float altura_maxima_util = 57.5f;

  // Se a distância medida for menor ou igual a 21.0 cm (limite prático do
  // sensor), forçamos a distância para 20.5 cm para que a altura do leite
  // seja 57.5 cm (550L).
  if (distancia <= 21.0f) {
    distancia = 20.5f;
  }

  // Calcula a altura do leite com base na distância medida
  float altura_leite = altura_sensor - distancia;

  // Limita entre 0 e a altura máxima útil (57.5 cm)
  if (altura_leite < 0.0f) {
    altura_leite = 0.0f;
  }
  if (altura_leite > altura_maxima_util) {
    altura_leite = altura_maxima_util;
  }

  // Calcula o volume do leite (em cm³) usando a interpolação linear por zonas
  float volume = calcular_volume_interpolado(altura_leite);
  return volume;
}

void exibir_no_oled(float dist_bruta, float dist_suave, float volume) {
  char linha1[24];
  char linha2[24];
  char linha3[24];

  snprintf(linha1, sizeof(linha1), "Bruta: %.1f cm", dist_bruta);
  snprintf(linha2, sizeof(linha2), "Suave: %.1f cm", dist_suave);
  snprintf(linha3, sizeof(linha3), "Vol  : %.1f L", volume / 1000.0f);

  ssd1306_Fill(Black);
  ssd1306_SetCursor(0, 8);
  ssd1306_WriteString(linha1, Font_7x10, White);
  ssd1306_SetCursor(0, 24);
  ssd1306_WriteString(linha2, Font_7x10, White);
  ssd1306_SetCursor(0, 40);
  ssd1306_WriteString(linha3, Font_7x10, White);

  ssd1306_UpdateScreen(); // Atualiza a tela OLED
}

int main() {
  stdio_init_all();

  gpio_init(TRIG_PIN);
  gpio_set_dir(TRIG_PIN, GPIO_OUT);

  gpio_init(ECHO_PIN);
  gpio_set_dir(ECHO_PIN, GPIO_IN);

  ssd1306_Init();

  int falhas_consecutivas = 0;
  while (true) {
    float distancia_bruta = medir_distancia_cm();
    if (distancia_bruta < 0.0f) {
      falhas_consecutivas++;
      if (falhas_consecutivas >= 3) {
        printf("Erro: sensor sem resposta\n");
        ssd1306_Fill(Black);
        ssd1306_SetCursor(0, 24);
        ssd1306_WriteString("Sensor sem", Font_7x10, White);
        ssd1306_SetCursor(0, 36);
        ssd1306_WriteString("resposta", Font_7x10, White);
        ssd1306_UpdateScreen();
        // Reseta a inicialização dos filtros ao perder o sinal
        buffer_inicializado = false;
        hist_mediana_inicializado = false;
      }
      sleep_ms(1000);
      continue;
    }

    falhas_consecutivas = 0; // Leitura válida reseta as falhas

    // Aplica a calibração linear baseada em regressão
    float distancia_calibrada = calibrar_distancia(distancia_bruta);

    // 1. Valida a leitura física e limita variações bruscas impossíveis
    float distancia_validada = filtrar_e_validar(distancia_calibrada);
    if (distancia_validada < 0.0f) {
      distancia_validada = distancia_calibrada;
    }

    // 2. Filtro de mediana para eliminar picos de ruído espúrios (outliers)
    float distancia_mediana = calcular_mediana(distancia_validada);

    // 3. Média móvel sobre o valor estável para suavizar oscilações térmicas ou
    // de superfície
    float distancia_suavizada = calcular_media_movel(distancia_mediana);

    float volume = volumeLeite(distancia_suavizada);
    printf("Bruta: %.2f cm | Calibrada: %.2f cm | Mediana: %.2f cm | Média: "
           "%.2f cm\n",
           distancia_bruta, distancia_calibrada, distancia_mediana,
           distancia_suavizada);
    printf("Volume: %.2f L\n", volume / 1000.0f);
    exibir_no_oled(distancia_bruta, distancia_suavizada, volume);
    sleep_ms(1000);
  }
}