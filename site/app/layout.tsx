import type { Metadata } from "next";
import { Space_Grotesk, Space_Mono } from "next/font/google";
import { headers } from "next/headers";
import "./globals.css";

const display = Space_Grotesk({ variable: "--font-display", subsets: ["latin"] });
const mono = Space_Mono({ variable: "--font-mono", subsets: ["latin"], weight: ["400", "700"] });

export async function generateMetadata(): Promise<Metadata> {
  const requestHeaders = await headers();
  const host = requestHeaders.get("x-forwarded-host") ?? requestHeaders.get("host") ?? "localhost:3000";
  const protocol = requestHeaders.get("x-forwarded-proto") ?? (host.startsWith("localhost") ? "http" : "https");
  const origin = `${protocol}://${host}`;
  const title = "Cardputer ADV Agent Console";
  const description = "Voice capture, Codex tasks, and private Obsidian delivery from a pocket terminal.";
  return {
    title,
    description,
    metadataBase: new URL(origin),
    openGraph: {
      title,
      description,
      type: "website",
      images: [{ url: `${origin}/images/official/cardputer-adv-angle.webp`, width: 1200, height: 1200, alt: "M5Stack Cardputer ADV" }],
    },
    twitter: { card: "summary_large_image", title, description, images: [`${origin}/images/official/cardputer-adv-angle.webp`] },
  };
}

export default function RootLayout({ children }: Readonly<{ children: React.ReactNode }>) {
  return <html lang="en"><body className={`${display.variable} ${mono.variable}`}>{children}</body></html>;
}
