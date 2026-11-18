import sys
import pandas as pd
import matplotlib.pyplot as plt

csv_path = sys.argv[1] if len(sys.argv) > 1 else "latency_results.csv"
output_img = sys.argv[2] if len(sys.argv) > 2 else "grafico_latencia_switch.png"

df = pd.read_csv(csv_path)

if df.empty:
    print("O arquivo CSV esta vazio.")
    sys.exit(1)

fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(15, 6))

for fsize, group in df.groupby("frame_size_bytes"):
    ax1.plot(
        group["elapsed_sec"].to_numpy(),
        group["latency_us"].to_numpy(),
        label=f"{fsize}B",
        alpha=0.8,
        linewidth=1,
    )

ax1.set_title("Latencia do Switch ao Longo do Tempo")
ax1.set_xlabel("Tempo decorrido (s)")
ax1.set_ylabel("Latencia (us)")
ax1.grid(True, linestyle="--", alpha=0.5)
ax1.legend(title="Tamanho do Quadro")

sizes = sorted(df["frame_size_bytes"].unique())
data_by_size = [df[df["frame_size_bytes"] == s]["latency_us"] for s in sizes]

ax2.boxplot(data_by_size, showfliers=False)
ax2.set_xticks(range(1, len(sizes) + 1))
ax2.set_xticklabels([f"{s}B" for s in sizes])
ax2.set_title("Distribuicao de Latencia por Tamanho de Pacote (sem outliers)")
ax2.set_xlabel("Tamanho do Quadro Ethernet")
ax2.set_ylabel("Latencia (us)")
ax2.grid(True, linestyle="--", alpha=0.5)

plt.tight_layout()
plt.savefig(output_img, dpi=150)
print(f"Grafico salvo em {output_img}")
