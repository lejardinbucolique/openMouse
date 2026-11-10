# openMouse : mode d'emploi

*[English version](HELP.md) · [Deutsche Version](HILFE.md)*

openMouse donne à chaque souris branchée sa propre flèche sur le même écran Windows.
Deux personnes peuvent ainsi travailler en même temps sur un seul ordinateur, chacune avec
sa souris : l'une clique ici, l'autre fait défiler une page là-bas.

## Ce qu'il faut savoir avant de commencer

- openMouse remplace la flèche de Windows par une flèche colorée par souris. Par défaut,
  la première est jaune, la deuxième orange.
- Une seule personne tape au clavier à la fois : Windows n'envoie les touches qu'à la
  fenêtre active. Les deux souris sont indépendantes, pas les claviers.
- Il faut Windows 10 ou 11 en 64 bits, et au moins deux souris (le pavé tactile d'un
  portable compte comme une souris).

## 1. Démarrer

1. Lancez `openmouse.exe`. Il n'y a pas d'installation.
2. Windows demande l'autorisation administrateur : acceptez. Sans elle, openMouse
   fonctionne quand même, mais les fenêtres lancées « en tant qu'administrateur »
   (le Gestionnaire des tâches par exemple) échappent aux souris.
3. Une icône apparaît près de l'horloge. Deux flèches de couleurs différentes sont
   à l'écran.

Pour que le programme démarre avec Windows : clic sur l'icône, **Réglages…**, onglet
**Général**, case **Lancer openMouse au démarrage de Windows**. Cette case n'est
disponible qu'une fois openMouse installé dans un dossier protégé comme `Program Files`,
car le programme démarre alors avec tous les droits ; depuis un autre dossier
(Téléchargements, Bureau…) elle est grisée et le dit.

## 2. Brancher deux souris

Branchez simplement la deuxième souris : une nouvelle flèche apparaît, d'une couleur
différente de la première. Débranchée, sa flèche disparaît ; rebranchée, elle revient
là où elle s'était arrêtée. La dernière flèche n'est jamais masquée, vous ne restez donc
jamais sans pointeur.

Chaque souris garde sa couleur, même branchée sur un autre port USB.

Si deux flèches sont inversées ou si une souris pilote la mauvaise flèche (cela peut
arriver avec des récepteurs sans fil), débranchez puis rebranchez le récepteur de la
souris concernée, et ouvrez **Réglages…** pour voir les souris reconnues.

## 3. Se servir des deux souris

- **Bouger** : chaque souris déplace sa flèche, sans gêner l'autre.
- **Cliquer** : le clic va là où pointe la flèche de la souris qui clique. Cliquer dans
  une fenêtre qui n'est pas active l'active et clique en une seule fois.
- **Faire défiler** : la molette fait défiler la fenêtre située sous sa propre flèche,
  sans clic préalable.
- **Glisser** (déplacer un fichier, sélectionner) : pendant qu'une personne maintient un
  bouton, les clics de l'autre attendent la fin du geste. Les flèches restent libres.
- **Boutons précédent et suivant** de la souris : ils fonctionnent aussi, chacun pour sa
  souris.

## 4. Les réglages

Clic sur l'icône près de l'horloge, puis **Réglages…**. La fenêtre a deux onglets
(**Ctrl + Tab** passe de l'un à l'autre). Chaque changement s'applique tout de suite et
s'enregistre ; **Annuler mes changements** remet les valeurs des souris à leur état
d'ouverture de la fenêtre. Ces deux boutons et la ligne d'état (« Appliqué et enregistré »)
sont en bas, communs aux deux onglets.

### Onglet Général

- **Lancer openMouse au démarrage de Windows** : voir « Démarrer » plus haut.
- **Langue** : « Automatique (langue de Windows) », « Français », « English » ou « Deutsch ». Le choix
  s'applique tout de suite à la fenêtre, au menu de l'icône et aux messages d'openMouse, et
  il est retenu.
- **Aide** : ouvre cette page d'aide, dans la langue choisie, sur GitHub.
- **À propos** : les crédits, avec le lien vers le site de Calepitool.

### Onglet Souris

Une zone par souris, et une pour le pavé tactile d'un portable. Passer la souris sur
**Sensibilité fixe**, **Accélération progressive** ou **Apprendre un bouton** affiche une
courte info-bulle.

### Reconnaître et mettre de côté une souris

Le titre de chaque zone donne la marque de la souris (« Souris Logitech »), lue sur la
souris elle-même ou, à défaut, d'après son fabricant. Quand la marque ne se lit pas, le
titre reste « Souris » suivi d'un numéro ; deux souris de même marque portent en plus
leur numéro.

La case **Désactiver**, à droite du titre, met la souris de côté sans la débrancher : sa
flèche disparaît et plus rien de ce qu'elle fait n'est pris en compte, tout de suite.
Décochée, elle revient là où elle s'était arrêtée, avec sa couleur et sa vitesse. On ne
peut pas désactiver la dernière souris active (le pavé tactile compte quand il est
actif) : openMouse le refuse, pour que vous ne restiez jamais sans pointeur.

### Apparence de la flèche

- **Couleur** : dix couleurs au choix, pour reconnaître sa flèche d'un coup d'œil.
- **Jeu de curseurs** : classique, liseré brillant (le plus visible, c'est le choix par
  défaut), corps blanc et bord coloré, grand, ou halo lumineux. L'aperçu montre le
  résultat sur quatre fonds.

### Vitesse de la flèche

Chaque souris suit l'un de ces deux modes (les curseurs de l'autre mode sont grisés) :

- **Sensibilité fixe** : tous les mouvements sont multipliés par la même valeur. C'est le
  plus simple : montez la valeur si la flèche est trop lente.
- **Accélération progressive** : lente pour les petits gestes (précision), rapide pour
  les grands (traverser l'écran). Le **seuil d'accélération** règle le moment où la flèche
  accélère.

**Valeurs par défaut** remet les réglages d'origine.

### Rester dans l'écran

Pour chaque souris, vous pouvez limiter la flèche à un seul écran (utile quand chaque
personne travaille sur son moniteur) ou la laisser parcourir tout le bureau.

### Apprendre un bouton

Une souris a parfois des boutons que Windows ne connaît pas. Cliquez sur **Apprendre un
bouton**, puis appuyez sur le bouton dans les huit secondes. Choisissez alors ce qu'il
fait : un clic (gauche, droit, milieu, précédent, suivant), un raccourci clavier, ou rien.
Le clic se fait à la position de la flèche de cette souris, jamais de celle d'une autre. **Oublier** retire le bouton
choisi.

Certains boutons ne se laissent pas apprendre : ceux que seul le logiciel du fabricant
sait lire n'envoient rien à Windows. La fenêtre vous le dit.

## 5. Si quelque chose ne va pas

| Problème | Que faire |
|---|---|
| Plus de flèche, ou la flèche reste cachée | Appuyez sur **Ctrl + Alt + Maj + Q** : openMouse s'arrête et rend la flèche de Windows. Sinon, lancez `openmouse.exe --restore-cursor`. |
| Après Ctrl + Alt + Suppr ou un verrouillage de session | C'est normal : Windows reprend la main. Au retour, openMouse se remet en route tout seul. |
| Une fenêtre d'administrateur ignore les souris | Relancez openMouse en acceptant l'autorisation administrateur. |
| Une flèche est suivie du mauvais pointeur | Rebranchez la souris, puis vérifiez dans **Réglages…** les souris reconnues. |
| Autre souci | **Ouvrir le dossier du journal** dans le menu de l'icône : le fichier `openmouse.log` dit ce qui s'est passé. |

## 6. Limites connues

- **Un seul clavier actif à la fois.** Les deux personnes ne peuvent pas taper en même
  temps dans deux fenêtres différentes.
- **Barre des tâches, menu Démarrer et notifications.** Seule la flèche de la souris qui
  « tient » le vrai pointeur Windows y est visible. L'autre flèche passe derrière ces
  éléments : pour s'en servir, cliquez d'abord avec cette souris dans une fenêtre
  ordinaire, ou laissez l'autre personne s'en occuper.
- **Un geste de glisser tout de suite dans une fenêtre inactive** est perdu : cliquez une
  première fois pour activer la fenêtre, puis glissez.
- **Défilement simultané** : si les deux personnes font défiler au même instant, un cran
  de molette peut parfois aller dans l'autre fenêtre.
- Le son, le presse-papiers et l'ordre des fenêtres restent communs aux deux personnes.

Pour le détail technique (compilation, options de ligne de commande, fichier de
configuration), voir le [README](../README.md).
